#include "ParametricEQEditor.h"

using namespace theme::colours;

static constexpr int controlsHeight = 92;

static juce::String hzText (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz"
                        : juce::String (juce::roundToInt (hz)) + " Hz";
}

static juce::String dbText (double db)   { return (db > 0.0 ? "+" : "") + juce::String (db, 1) + " dB"; }

juce::Colour ParametricEQEditor::bandColour (int band)   { return theme::channelColour (band); }

//==============================================================================
ParametricEQEditor::ParametricEQEditor (ParametricEQPlugin& p)
    : plugin (p)
{
    using A = ParametricEQPlugin::Analyser;
    fft = std::make_unique<juce::dsp::FFT> (A::fftOrder);
    fftData.assign ((size_t) A::fftSize * 2, 0.0f);
    ring.assign ((size_t) A::fftSize, 0.0f);
    window.resize ((size_t) A::fftSize);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) A::fftSize,
                                                              juce::dsp::WindowingFunction<float>::hann, false);

    for (int i = 0; i < peq::numBands; ++i)
        lastOnType[(size_t) i] = ParametricEQPlugin::defaultBand (i).type;

    // Band buttons 1..7
    for (int i = 0; i < peq::numBands; ++i)
    {
        auto* b = bandButtons.add (new juce::TextButton (juce::String (i + 1)));
        b->setTooltip (TRANS ("Band") + " " + juce::String (i + 1));
        b->onClick = [this, i] { selectBand (i); };
        addAndMakeVisible (b);
    }

    for (int t = 0; t < peq::numTypes; ++t)
        typeBox.addItem (TRANS (peq::typeName (t)), t + 1);

    for (int o = 0; o < 4; ++o)
        slopeBox.addItem (juce::String (peq::dbPerOctave (o)) + " dB/oct", o + 1);

    typeBox.setTooltip (TRANS ("Band type"));
    slopeBox.setTooltip (TRANS ("Slope: how steep the band is (cuts, shelves, notch, band pass)"));

    typeBox.onChange = [this]
    {
        if (updatingControls || typeBox.getSelectedId() == 0)
            return;

        auto b = plugin.getBands()[(size_t) selected];
        b.type = typeBox.getSelectedId() - 1;
        beginEdit();
        setBand (selected, b);
    };

    slopeBox.onChange = [this]
    {
        if (updatingControls || slopeBox.getSelectedId() == 0)
            return;

        auto b = plugin.getBands()[(size_t) selected];
        b.order = slopeBox.getSelectedId() - 1;
        beginEdit();
        setBand (selected, b);
    };

    addAndMakeVisible (typeBox);
    addAndMakeVisible (slopeBox);

    auto setUpSlider = [this] (ResetSlider& s, juce::Label& label, const juce::String& name)
    {
        label.setText (name, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, muted);
        label.setFont (theme::ui (12.0f, theme::Weight::medium));
        label.setJustificationType (juce::Justification::centredRight);
        s.setColour (juce::Slider::trackColourId, accent);
        s.setColour (juce::Slider::textBoxTextColourId, text);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
        s.onDragStart = [this] { beginEdit(); };
        addAndMakeVisible (s);
        addAndMakeVisible (label);
    };

    setUpSlider (freqSlider, freqLabel, TRANS ("Freq"));
    setUpSlider (gainSlider, gainLabel, TRANS ("Gain"));
    setUpSlider (qSlider, qLabel, TRANS ("Width (Q)"));
    setUpSlider (mainSlider, mainLabel, TRANS ("Main level"));

    juce::NormalisableRange<double> fr (10.0, 22000.0);
    fr.setSkewForCentre (630.0);
    freqSlider.setNormalisableRange (fr);
    freqSlider.textFromValueFunction = [] (double v) { return hzText (v); };
    freqSlider.valueFromTextFunction = [] (const juce::String& s)
    {
        const double v = s.getDoubleValue();
        return s.containsIgnoreCase ("k") ? v * 1000.0 : v;
    };

    gainSlider.setRange (-18.0, 18.0, 0.1);
    gainSlider.textFromValueFunction = [] (double v) { return dbText (v); };
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.describeDefault = [] { return juce::String ("0 dB"); };

    juce::NormalisableRange<double> qr (0.1, 24.0);
    qr.setSkewForCentre (1.0);
    qSlider.setNormalisableRange (qr);
    qSlider.textFromValueFunction = [] (double v) { return "Q " + juce::String (v, 2); };

    mainSlider.setRange (-24.0, 12.0, 0.1);
    mainSlider.textFromValueFunction = [] (double v) { return dbText (v); };
    mainSlider.setDoubleClickReturnValue (true, 0.0);
    mainSlider.describeDefault = [] { return juce::String ("0 dB"); };

    auto bandEdit = [this] (std::function<void (peq::Band&)> change)
    {
        if (updatingControls)
            return;

        auto b = plugin.getBands()[(size_t) selected];
        change (b);
        setBand (selected, b);
    };

    freqSlider.onValueChange = [this, bandEdit] { bandEdit ([this] (peq::Band& b) { b.freq = freqSlider.getValue(); }); };
    gainSlider.onValueChange = [this, bandEdit] { bandEdit ([this] (peq::Band& b) { b.gain = gainSlider.getValue(); }); };
    qSlider.onValueChange    = [this, bandEdit] { bandEdit ([this] (peq::Band& b) { b.q = qSlider.getValue(); }); };
    mainSlider.onValueChange = [this]
    {
        if (! updatingControls)
            plugin.mainParam->setParameter ((float) mainSlider.getValue(), juce::sendNotification);
    };

    // The text format only shows once the value changes: refresh it now ("0.0 dB", not "0.0").
    for (auto* s : { &freqSlider, &gainSlider, &qSlider, &mainSlider })
        s->updateText();

    setSize (880, 480);
    plugin.analyser.wanted = true;
    shown = plugin.getBands();
    shownMain = plugin.getMainDb();
    selectBand (3);
    startTimerHz (30);
}

ParametricEQEditor::~ParametricEQEditor()
{
    plugin.analyser.wanted = false;
    plugin.setSoloBand (-1);
}

//==============================================================================
juce::Rectangle<int> ParametricEQEditor::graphArea() const
{
    return getLocalBounds().withTrimmedBottom (controlsHeight).reduced (12).withTrimmedLeft (30).withTrimmedBottom (16);
}

float ParametricEQEditor::xForFreq (double hz) const
{
    auto g = graphArea().toFloat();
    return g.getX() + g.getWidth() * (float) (std::log (juce::jmax (1.0, hz) / minHz) / std::log (maxHz / minHz));
}

double ParametricEQEditor::freqForX (float x) const
{
    auto g = graphArea().toFloat();
    return minHz * std::exp ((double) ((x - g.getX()) / g.getWidth()) * std::log (maxHz / minHz));
}

float ParametricEQEditor::yForDb (double db) const
{
    auto g = graphArea().toFloat();
    return g.getCentreY() - (float) (db / dbRange) * (g.getHeight() * 0.5f - 6.0f);
}

double ParametricEQEditor::dbForY (float y) const
{
    auto g = graphArea().toFloat();
    return (double) ((g.getCentreY() - y) / (g.getHeight() * 0.5f - 6.0f)) * dbRange;
}

juce::Point<float> ParametricEQEditor::pointFor (int band) const
{
    const auto& b = shown[(size_t) band];
    return { xForFreq (b.freq), yForDb (peq::hasGain (b.type) ? b.gain : 0.0) };
}

int ParametricEQEditor::bandAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDistance = 12.0f;

    // The selected band wins when points overlap.
    for (int k = 0; k < peq::numBands; ++k)
    {
        const int i = (selected + k) % peq::numBands;
        const float d = pointFor (i).getDistanceFrom (p);

        if (d < bestDistance - (k == 0 ? 0.0f : 0.5f))
        {
            best = i;
            bestDistance = d;
        }
    }

    return best;
}

//==============================================================================
void ParametricEQEditor::timerCallback()
{
    updateSpectrum();

    const auto now = plugin.getBands();
    const double main = plugin.getMainDb();

    if (now != shown || main != shownMain)
    {
        shown = now;
        shownMain = main;
        updateCurve();
        syncControls();
    }

    repaint (graphArea().expanded (40, 12));
}

void ParametricEQEditor::updateCurve()
{
    auto g = graphArea().toFloat();
    const double sr = plugin.getSampleRate();
    curve.clear();

    bool first = true;   // (Path::isEmpty() stays true while a path has only its start point)

    for (float x = g.getX(); x <= g.getRight(); x += 2.0f)
    {
        const double db = peq::responseDb (shown, 0.0, freqForX (x), sr);
        const float y = juce::jlimit (g.getY() - 4.0f, g.getBottom() + 4.0f, yForDb (db));

        if (first)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);

        first = false;
    }
}

void ParametricEQEditor::updateSpectrum()
{
    using A = ParametricEQPlugin::Analyser;
    auto& fifo = plugin.analyser.fifo;
    const int ready = fifo.getNumReady();

    if (ready > 0)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (ready, s1, n1, s2, n2);

        auto copy = [this] (const float* src, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                ring[(size_t) ringPos] = src[i];
                ringPos = (ringPos + 1) % A::fftSize;
            }
        };

        copy (plugin.analyser.buffer.data() + s1, n1);
        copy (plugin.analyser.buffer.data() + s2, n2);
        fifo.finishedRead (n1 + n2);
    }

    auto g = graphArea().toFloat();
    const int columns = juce::jmax (2, (int) (g.getWidth() / 3.0f));

    if ((int) spectrumDb.size() != columns)
        spectrumDb.assign ((size_t) columns, -120.0f);

    const double sr = plugin.getSampleRate();

    if (ready > 0)
    {
        for (int i = 0; i < A::fftSize; ++i)
            fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % A::fftSize)] * window[(size_t) i];

        std::fill (fftData.begin() + A::fftSize, fftData.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

        const double binHz = sr / A::fftSize;
        const float norm = 4.0f / (float) A::fftSize;   // a full-scale sine reads about 0 dB

        for (int c = 0; c < columns; ++c)
        {
            const double f0 = freqForX (g.getX() + g.getWidth() * (float) c / (float) columns);
            const double f1 = freqForX (g.getX() + g.getWidth() * (float) (c + 1) / (float) columns);
            const int b0 = juce::jlimit (1, A::fftSize / 2 - 1, (int) std::floor (f0 / binHz));
            const int b1 = juce::jlimit (b0, A::fftSize / 2 - 1, (int) std::ceil (f1 / binHz));
            float peak = 0.0f;

            for (int b = b0; b <= b1; ++b)
                peak = juce::jmax (peak, fftData[(size_t) b]);

            // Tilt 4.5 dB per octave around 1 kHz so a typical mix looks flat (display only).
            const float tilt = 4.5f * (float) std::log2 (juce::jmax (1.0, 0.5 * (f0 + f1)) / 1000.0);
            const float db = juce::Decibels::gainToDecibels (peak * norm, -120.0f) + tilt;
            auto& s = spectrumDb[(size_t) c];
            s = db > s ? db : juce::jmax (db, s - 1.5f);   // rise at once, fall slowly
        }
    }
    else
    {
        for (auto& s : spectrumDb)
            s = juce::jmax (-120.0f, s - 1.5f);   // sound stopped: let it fall away
    }

    // Spectrum scale: 0 dB at the top, -90 dB at the bottom.
    spectrumPath.clear();
    spectrumPath.startNewSubPath (g.getX(), g.getBottom());

    for (int c = 0; c < columns; ++c)
    {
        const float x = g.getX() + g.getWidth() * ((float) c + 0.5f) / (float) columns;
        const float level = juce::jlimit (0.0f, 1.0f, (spectrumDb[(size_t) c] + 90.0f) / 90.0f);
        spectrumPath.lineTo (x, g.getBottom() - level * g.getHeight());
    }

    spectrumPath.lineTo (g.getRight(), g.getBottom());
    spectrumPath.closeSubPath();
}

//==============================================================================
void ParametricEQEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    auto area = graphArea().toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (area.expanded (4.0f), 6.0f);

    // Grid: frequencies
    g.setFont (theme::mono (10.0f));

    for (double hz : { 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 200.0, 300.0, 400.0, 500.0, 600.0, 700.0, 800.0, 900.0,
                       1000.0, 2000.0, 3000.0, 4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9000.0, 10000.0, 20000.0 })
    {
        const bool major = hz == 50.0 || hz == 100.0 || hz == 200.0 || hz == 500.0 || hz == 1000.0
                            || hz == 2000.0 || hz == 5000.0 || hz == 10000.0;
        const float x = xForFreq (hz);
        g.setColour (major ? gridBar : grid);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());

        if (major)
        {
            g.setColour (dim);
            g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                        (int) x - 20, (int) area.getBottom() + 4, 40, 12, juce::Justification::centred, false);
        }
    }

    // Grid: gain
    for (int db = -18; db <= 18; db += 6)
    {
        const float y = yForDb (db);
        g.setColour (db == 0 ? gridBar.brighter (0.2f) : grid);
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (dim);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), (int) area.getX() - 38, (int) y - 6, 30, 12,
                    juce::Justification::centredRight, false);
    }

    // Spectrum (what comes out of the EQ)
    g.setColour (accent.withAlpha (0.10f));
    g.fillPath (spectrumPath);
    g.setColour (accent.withAlpha (0.22f));
    g.strokePath (spectrumPath, juce::PathStrokeType (1.0f));

    g.saveState();
    g.reduceClipRegion (area.toNearestInt());

    // The selected band's own shape
    const auto& sel = shown[(size_t) selected];
    const double sr = plugin.getSampleRate();

    if (sel.type != peq::off)
    {
        juce::Path band;
        const float zeroY = yForDb (0.0);
        band.startNewSubPath (area.getX(), zeroY);

        for (float x = area.getX(); x <= area.getRight(); x += 3.0f)
            band.lineTo (x, juce::jlimit (area.getY() - 4.0f, area.getBottom() + 4.0f,
                                          yForDb (peq::bandResponseDb (sel, freqForX (x), sr))));

        band.lineTo (area.getRight(), zeroY);
        band.closeSubPath();
        g.setColour (bandColour (selected).withAlpha (0.16f));
        g.fillPath (band);
    }

    // The whole EQ curve

    g.setColour (lilac);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
    g.restoreState();

    // Band points
    for (int i = 0; i < peq::numBands; ++i)
    {
        const auto& b = shown[(size_t) i];
        const auto p = pointFor (i);
        const bool on = b.type != peq::off;
        const bool hot = i == selected || i == hovered || i == dragging;
        auto colour = bandColour (i);
        auto r = juce::Rectangle<float> (18.0f, 18.0f).withCentre (p);

        if (on)
        {
            g.setColour (colour);
            g.fillEllipse (r);
        }
        else
        {
            g.setColour (lcd);
            g.fillEllipse (r);
            g.setColour (colour.withAlpha (0.5f));
            g.drawEllipse (r.reduced (1.0f), 1.5f);
        }

        if (hot)
        {
            g.setColour (text);
            g.drawEllipse (r.expanded (2.0f), 1.5f);
        }

        g.setColour (on ? accentText : colour.withAlpha (0.7f));
        g.setFont (theme::ui (11.0f, theme::Weight::bold));
        g.drawText (juce::String (i + 1), r, juce::Justification::centred, false);

        if (i == plugin.getSoloBand())
        {
            g.setColour (amber);
            g.drawEllipse (r.expanded (5.0f), 1.5f);
        }
    }

    // Read-out next to the band under the mouse (or being dragged)
    const int info = dragging >= 0 ? dragging : hovered;

    if (info >= 0)
    {
        const auto& b = shown[(size_t) info];
        auto p = pointFor (info);
        juce::String s = TRANS (peq::typeName (b.type)) + "   " + hzText (b.freq);

        if (peq::hasGain (b.type))
            s << "   " << dbText (b.gain);

        if (b.type != peq::off)
            s << "   Q " << juce::String (b.q, 2);

        const auto font = theme::ui (12.0f, theme::Weight::semibold);
        g.setFont (font);
        const int w = juce::GlyphArrangement::getStringWidthInt (font, s) + 16;
        auto box = juce::Rectangle<int> (w, 22).withCentre ({ (int) p.x, (int) p.y - 26 });
        box = box.constrainedWithin (area.toNearestInt());
        g.setColour (raised.withAlpha (0.95f));
        g.fillRoundedRectangle (box.toFloat(), 4.0f);
        g.setColour (text);
        g.drawText (s, box, juce::Justification::centred, false);
    }

    // Separator above the controls
    g.setColour (line);
    g.fillRect (0, getHeight() - controlsHeight, getWidth(), 1);
}

void ParametricEQEditor::resized()
{
    auto r = getLocalBounds().removeFromBottom (controlsHeight).reduced (12, 10);
    auto row1 = r.removeFromTop (30);
    r.removeFromTop (8);
    auto row2 = r.removeFromTop (30);

    for (auto* b : bandButtons)
    {
        b->setBounds (row1.removeFromLeft (30));
        row1.removeFromLeft (4);
    }

    row1.removeFromLeft (12);
    typeBox.setBounds (row1.removeFromLeft (200));
    row1.removeFromLeft (8);
    slopeBox.setBounds (row1.removeFromLeft (120));
    mainSlider.setBounds (row1.removeFromRight (220));
    mainLabel.setBounds (row1.removeFromRight (80));

    const int third = row2.getWidth() / 3;

    for (auto* pair : { &freqSlider, &gainSlider, &qSlider })
    {
        auto cell = row2.removeFromLeft (third);
        auto& label = pair == &freqSlider ? freqLabel : pair == &gainSlider ? gainLabel : qLabel;
        label.setBounds (cell.removeFromLeft (70));
        pair->setBounds (cell.reduced (4, 0));
    }

    updateCurve();
}

//==============================================================================
void ParametricEQEditor::selectBand (int band)
{
    selected = juce::jlimit (0, peq::numBands - 1, band);

    for (int i = 0; i < bandButtons.size(); ++i)
    {
        auto* b = bandButtons[i];
        const bool on = i == selected;
        b->setColour (juce::TextButton::buttonColourId, on ? bandColour (i) : well);
        b->setColour (juce::TextButton::textColourOffId, on ? accentText : bandColour (i));
    }

    syncControls();
    repaint();
}

void ParametricEQEditor::syncControls()
{
    const juce::ScopedValueSetter<bool> svs (updatingControls, true);
    const auto& b = shown[(size_t) selected];

    typeBox.setSelectedId (b.type + 1, juce::dontSendNotification);
    slopeBox.setSelectedId (b.order + 1, juce::dontSendNotification);
    slopeBox.setEnabled (b.type != peq::off && b.type != peq::peak);
    gainSlider.setEnabled (peq::hasGain (b.type));
    qSlider.setEnabled (b.type != peq::off);

    const auto d = ParametricEQPlugin::defaultBand (selected);
    freqSlider.setDoubleClickReturnValue (true, d.freq);
    freqSlider.describeDefault = [d] { return hzText (d.freq); };
    qSlider.setDoubleClickReturnValue (true, d.q);
    qSlider.describeDefault = [d] { return "Q " + juce::String (d.q, 2); };

    if (! freqSlider.isMouseButtonDown())  freqSlider.setValue (b.freq, juce::dontSendNotification);
    if (! gainSlider.isMouseButtonDown())  gainSlider.setValue (b.gain, juce::dontSendNotification);
    if (! qSlider.isMouseButtonDown())     qSlider.setValue (b.q, juce::dontSendNotification);
    if (! mainSlider.isMouseButtonDown())  mainSlider.setValue (shownMain, juce::dontSendNotification);
}

void ParametricEQEditor::beginEdit()
{
    plugin.edit.getUndoManager().beginNewTransaction();
}

void ParametricEQEditor::setBand (int index, const peq::Band& b)
{
    // Only the values that change are written (each one is sent to the other PCs).
    const auto now = plugin.getBands()[(size_t) index];
    auto& p = plugin.bands[(size_t) index];

    if (b.type != now.type)    p.type->setParameter ((float) b.type, juce::sendNotification);
    if (b.order != now.order)  p.order->setParameter ((float) b.order, juce::sendNotification);
    if (b.freq != now.freq)    p.freq->setParameter ((float) juce::jlimit (10.0, 22000.0, b.freq), juce::sendNotification);
    if (b.gain != now.gain)    p.gain->setParameter ((float) juce::jlimit (-18.0, 18.0, b.gain), juce::sendNotification);
    if (b.q != now.q)          p.q->setParameter ((float) juce::jlimit (0.1, 24.0, b.q), juce::sendNotification);

    if (b.type != peq::off)
        lastOnType[(size_t) index] = b.type;
}

void ParametricEQEditor::setSolo (bool on)
{
    plugin.setSoloBand (on ? (dragging >= 0 ? dragging : selected) : -1);
    repaint();
}

void ParametricEQEditor::showBandMenu (int band)
{
    const auto b = plugin.getBands()[(size_t) band];
    juce::PopupMenu m, slope;
    m.addSectionHeader (TRANS ("Band") + " " + juce::String (band + 1));

    for (int t = 1; t < peq::numTypes; ++t)
        m.addItem (1 + t, TRANS (peq::typeName (t)), true, b.type == t);

    for (int o = 0; o < 4; ++o)
        slope.addItem (100 + o, juce::String (peq::dbPerOctave (o)) + " dB/oct", true, b.order == o);

    m.addSubMenu (TRANS ("Slope"), slope, b.type != peq::off && b.type != peq::peak);
    m.addSeparator();
    m.addItem (1, b.type == peq::off ? TRANS ("Turn the band on") : TRANS ("Turn the band off"));
    m.addItem (200, TRANS ("Reset the band (Alt+click)"));

    juce::Component::SafePointer<ParametricEQEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, band] (int r)
    {
        if (safe == nullptr || r == 0)
            return;

        auto nb = safe->plugin.getBands()[(size_t) band];

        if (r == 1)               nb.type = nb.type == peq::off ? safe->lastOnType[(size_t) band] : (int) peq::off;
        else if (r >= 2 && r < 2 + peq::numTypes)  nb.type = r - 1;
        else if (r >= 100 && r < 104)              nb.order = r - 100;
        else if (r == 200)        nb = ParametricEQPlugin::defaultBand (band);

        safe->beginEdit();
        safe->setBand (band, nb);
    });
}

//==============================================================================
void ParametricEQEditor::mouseMove (const juce::MouseEvent& e)
{
    const int h = bandAt (e.position);

    if (h != hovered)
    {
        hovered = h;
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ParametricEQEditor::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

void ParametricEQEditor::mouseDown (const juce::MouseEvent& e)
{
    const int b = bandAt (e.position);

    if (b < 0)
        return;

    selectBand (b);

    if (e.mods.isPopupMenu())
    {
        showBandMenu (b);
        return;
    }

    if (e.mods.isAltDown())
    {
        beginEdit();
        setBand (b, ParametricEQPlugin::defaultBand (b));
        return;
    }

    const auto band = plugin.getBands()[(size_t) b];
    dragging = b;
    dragStart = e.position;
    dragStartFreq = band.freq;
    dragStartGain = band.gain;
    beginEdit();

    if (e.mods.isShiftDown())
        setSolo (true);
}

void ParametricEQEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    // Ctrl: fine adjustment (a tenth of the movement).
    const double scale = e.mods.isCtrlDown() ? 0.1 : 1.0;
    const auto g = graphArea().toFloat();
    const double octavesPerPixel = std::log2 (maxHz / minHz) / g.getWidth();
    const double dbPerPixel = dbRange / (g.getHeight() * 0.5 - 6.0);

    auto b = plugin.getBands()[(size_t) dragging];
    b.freq = juce::jlimit (10.0, 22000.0, dragStartFreq * std::pow (2.0, (e.position.x - dragStart.x) * octavesPerPixel * scale));

    if (peq::hasGain (b.type))
        b.gain = juce::jlimit (-18.0, 18.0, dragStartGain - (e.position.y - dragStart.y) * dbPerPixel * scale);

    setBand (dragging, b);
}

void ParametricEQEditor::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
    setSolo (false);
}

void ParametricEQEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = bandAt (e.position);

    if (b < 0)
        return;

    auto nb = plugin.getBands()[(size_t) b];
    nb.type = nb.type == peq::off ? lastOnType[(size_t) b] : (int) peq::off;
    beginEdit();
    setBand (b, nb);
}

void ParametricEQEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int b = bandAt (e.position);

    if (b < 0 && graphArea().contains (e.getPosition()))
        b = selected;

    if (b < 0 || w.deltaY == 0.0f)
        return;

    // One undo step per burst of scrolling.
    const auto now = juce::Time::getMillisecondCounter();

    if (now - lastWheelTime > 600)
        beginEdit();

    lastWheelTime = now;
    auto nb = plugin.getBands()[(size_t) b];
    nb.q = juce::jlimit (0.1, 24.0, nb.q * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));   // up = narrower
    selectBand (b);
    setBand (b, nb);
}
