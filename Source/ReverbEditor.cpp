#include "ReverbEditor.h"
#include <complex>

using namespace theme::colours;

static constexpr int knobW = 76, knobH = 82, titleH = 24, choiceH = 24, pad = 10, gap = 8;

ReverbEditor::ReverbEditor (ReverbPlugin& p) : plugin (p)
{
    sideChoice = std::make_unique<fxui::ParamChoice> (*plugin.side, juce::StringArray { TRANS ("Mid"), TRANS ("Side") }, teal);
    tempoToggle = std::make_unique<fxui::ParamToggle> (*plugin.tempo, TRANS ("Tempo"), teal);
    sideChoice->setTooltip (TRANS ("Mid: the reverb takes the summed (mono) sound. Side: only what differs between left and right"));
    tempoToggle->setTooltip (TRANS ("Pre-delay in note values that follow the song's tempo"));
    addAndMakeVisible (*sideChoice);
    addAndMakeVisible (*tempoToggle);

    // Room
    add (plugin.size, TRANS ("Size"), teal);
    add (plugin.diffusion, TRANS ("Diffusion"), teal)->setTooltip (TRANS ("Low: separate echoes. High: a dense, smooth tail"));
    predelayKnob = add (plugin.predelay, TRANS ("Pre-delay"), teal);
    divisionKnob = add (plugin.predelayDivision, TRANS ("Pre-delay"), teal);

    // Decay
    add (plugin.decay, TRANS ("Decay"), lilac)->setTooltip (TRANS ("Decay (RT60): the time the tail takes to fall by 60 dB"));
    add (plugin.bass, TRANS ("Bass"), lilac)->setTooltip (TRANS ("Bass multiplier: how much longer (or shorter) the lows last"));
    add (plugin.crossover, TRANS ("Cross"), lilac)->setTooltip (TRANS ("Crossover: the bass multiplier works below this frequency"));
    add (plugin.damp, TRANS ("Damp"), lilac)->setTooltip (TRANS ("High damping: the highs fade faster above this frequency (top = off)"));

    // Filters, modulation, mix
    add (plugin.lowCut, TRANS ("L. cut"), sky)->setTooltip (TRANS ("Low cut"));
    add (plugin.highCut, TRANS ("H. cut"), sky)->setTooltip (TRANS ("High cut"));
    add (plugin.mod, TRANS ("Mod"), mint)->setTooltip (TRANS ("Modulation: moves the tail slightly to avoid a metallic ring"));
    add (plugin.modSpeed, TRANS ("Speed"), mint);
    add (plugin.dry, TRANS ("Dry"), pink);
    add (plugin.early, TRANS ("ER"), pink)->setTooltip (TRANS ("Early reflections: the first echoes off the walls"));
    add (plugin.wet, TRANS ("Wet"), pink);
    add (plugin.separation, TRANS ("Sep"), pink)->setTooltip (TRANS ("Stereo separation of the reverb (the dry sound is not touched)"));

    tempoToggle->onChange = [this] { updatePredelayKnob(); };
    updatePredelayKnob();

    const int width = 2 * 12 + 8 * knobW + 3 * 2 * pad + 2 * gap + 24;
    setSize (width, 12 + 170 + 10 + 2 * (titleH + choiceH + knobH + 12) + gap + 12);
    startTimerHz (15);
}

fxui::ParamKnob* ReverbEditor::add (te::AutomatableParameter* p, const juce::String& label, juce::Colour c)
{
    auto* k = knobs.add (new fxui::ParamKnob (*p, label, c));
    addAndMakeVisible (k);
    return k;
}

void ReverbEditor::updatePredelayKnob()
{
    const bool synced = plugin.tempo->getCurrentValue() >= 0.5f;
    predelayKnob->setVisible (! synced);
    divisionKnob->setVisible (synced);
    repaint();
}

void ReverbEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    display = r.removeFromTop (170);
    r.removeFromTop (10);

    const int rowH = titleH + choiceH + knobH + 12;
    auto rowA = r.removeFromTop (rowH);
    r.removeFromTop (gap);
    auto rowB = r.removeFromTop (rowH);

    // Row A: room (3 knobs) | decay (4 knobs), shared out by knob count.
    roomArea = rowA.removeFromLeft ((rowA.getWidth() - gap) * 3 / 7);
    rowA.removeFromLeft (gap);
    decayArea = rowA;

    // Row B: filters (2) | modulation (2) | mix (4).
    const int unit = (rowB.getWidth() - 2 * gap) / 8;
    filterArea = rowB.removeFromLeft (2 * unit);
    rowB.removeFromLeft (gap);
    modArea = rowB.removeFromLeft (2 * unit);
    rowB.removeFromLeft (gap);
    mixArea = rowB;

    auto knobRow = [] (juce::Rectangle<int> area) { return area.withTrimmedTop (titleH + choiceH + 4).reduced (pad, 0).withHeight (knobH); };
    auto place = [&] (juce::Rectangle<int> area, std::initializer_list<int> which)
    {
        auto k = knobRow (area);
        const int w = k.getWidth() / (int) which.size();
        for (auto i : which) knobs[i]->setBounds (k.removeFromLeft (w));
    };

    auto c = roomArea.withTrimmedTop (titleH).reduced (pad, 0).withHeight (choiceH - 2);
    sideChoice->setBounds (c.removeFromLeft (120));
    c.removeFromLeft (8);
    tempoToggle->setBounds (c.removeFromLeft (80));

    place (roomArea, { 0, 1, 2 });
    knobs[3]->setBounds (knobs[2]->getBounds());   // pre-delay in ms or in note values
    place (decayArea, { 4, 5, 6, 7 });
    place (filterArea, { 8, 9 });
    place (modArea, { 10, 11 });
    place (mixArea, { 12, 13, 14, 15 });
}

void ReverbEditor::timerCallback()
{
    juce::String key;

    for (auto* p : plugin.params())
        key << p->getCurrentValue() << ";";

    if (key != shownKey)
    {
        shownKey = key;
        repaint (display);
    }
}

void ReverbEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    const auto s = plugin.getSettings();
    auto d = display.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (d, 6.0f);

    auto plot = display.reduced (14, 10).withTrimmedTop (20).withTrimmedLeft (22).withTrimmedBottom (12).toFloat();
    const double pre = s.predelaySeconds();
    const double rt = s.decaySeconds;
    const double rtBass = rt * s.bassMultiplier;

    // How long 10 kHz lasts: the damping filter takes a bit more off on every trip round a line.
    double rtHigh = rt;

    if (s.dampHz < 19999.0)
    {
        const double sr = 48000.0;
        const double c = rvb::onePoleCoef (s.dampHz, sr);
        const std::complex<double> z1 = std::polar (1.0, -juce::MathConstants<double>::twoPi * 10000.0 / sr);
        const double mag = std::abs (c / (1.0 - (1.0 - c) * z1));
        double avgMs = 0.0;
        for (auto ms : rvb::lineMs) avgMs += ms;
        avgMs = avgMs / rvb::numLines * rvb::sizeScale (s.size);
        const double extraDbPerSecond = -20.0 * std::log10 (juce::jmax (1.0e-6, mag)) / (avgMs * 0.001);
        rtHigh = 60.0 / (60.0 / rt + extraDbPerSecond);
    }

    const double span = juce::jlimit (1.0, 30.0, pre + 1.1 * juce::jmax (rt, rtBass));
    auto xFor = [&plot, span] (double t) { return plot.getX() + (float) (t / span) * plot.getWidth(); };
    auto yFor = [&plot] (double db) { return plot.getY() + (float) (juce::jlimit (0.0, 66.0, -db) / 66.0) * plot.getHeight() * 1.0f; };

    // Grid: dB rows, seconds columns
    g.setFont (theme::mono (9.5f));

    for (int db = 0; db >= -60; db -= 20)
    {
        g.setColour (db == 0 ? gridBar : grid);
        g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());
        g.setColour (dim);
        g.drawText (juce::String (db), (int) plot.getX() - 34, (int) yFor (db) - 6, 30, 12, juce::Justification::centredRight, false);
    }

    const double step = span <= 2.0 ? 0.25 : span <= 6.0 ? 1.0 : span <= 15.0 ? 2.0 : 5.0;

    for (double t = 0.0; t <= span + 1.0e-9; t += step)
    {
        const float x = xFor (t);
        g.setColour (grid);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (dim);
        g.drawText (juce::String (t, step < 1.0 ? 2 : 0) + " s", (int) x - 24, (int) plot.getBottom() + 1, 48, 11, juce::Justification::centred, false);
    }

    // Late tail: from the end of the pre-delay down to -60 dB in rt seconds.
    const float wetTop = (float) juce::jlimit (-60.0, 0.0, (double) juce::Decibels::gainToDecibels ((float) s.wet, -60.0f));
    auto tailLine = [&] (double seconds, juce::Colour c, bool fill)
    {
        juce::Path p;
        const double t0 = pre + 0.01;
        const double t60 = t0 + seconds * (60.0 + wetTop) / 60.0;   // a quieter wet starts lower
        p.startNewSubPath (xFor (t0), yFor (wetTop));
        p.lineTo (xFor (juce::jmin (span, t60)), yFor (t60 > span ? wetTop - (60.0 + wetTop) * (span - t0) / (t60 - t0) : -60.0));

        if (fill)
        {
            juce::Path area (p);
            area.lineTo (area.getCurrentPosition().x, plot.getBottom());
            area.lineTo (xFor (t0), plot.getBottom());
            area.closeSubPath();
            g.setGradientFill (juce::ColourGradient (c.withAlpha (0.45f), xFor (t0), plot.getY(), c.withAlpha (0.05f), xFor (t60), plot.getBottom(), false));
            g.fillPath (area);
        }

        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (fill ? 2.0f : 1.4f));
    };

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt().expanded (2));

    if (s.wet > 0.0)
    {
        tailLine (rt, lilac, true);

        if (std::abs (s.bassMultiplier - 1.0) > 0.01)
            tailLine (rtBass, amber, false);

        if (rtHigh < rt * 0.99)
            tailLine (rtHigh, sky, false);
    }

    // Early reflections: the taps after the pre-delay
    if (s.early > 0.0)
    {
        const double scale = rvb::sizeScale (s.size);

        for (auto& tap : rvb::tapsL)
        {
            const double t = pre + tap.ms * 0.001 * scale;
            const double db = juce::Decibels::gainToDecibels (std::abs (tap.gain) * s.early);
            const float x = xFor (t);
            g.setColour (teal.withAlpha (0.9f));
            g.fillRect (x - 1.0f, yFor (db), 2.0f, plot.getBottom() - yFor (db));
        }
    }

    // Dry at 0
    if (s.dry > 0.0)
    {
        g.setColour (text.withAlpha (0.85f));
        const float y = yFor (juce::Decibels::gainToDecibels ((float) s.dry, -60.0f));
        g.fillRect (xFor (0.0) - 1.5f, y, 3.0f, plot.getBottom() - y);
    }

    g.restoreState();

    // Read-out
    auto line = display.reduced (14, 6).removeFromTop (16);
    g.setFont (theme::mono (11.5f, true));
    g.setColour (text);
    juce::String readout = "RT60 " + juce::String (rt, rt < 10.0 ? 2 : 1) + " s";
    readout << "    " << TRANS ("pre-delay") << " " << juce::String (juce::roundToInt (pre * 1000.0)) << " ms";
    g.drawText (readout, line, juce::Justification::centredLeft, false);

    g.setFont (theme::ui (11.0f, theme::Weight::semibold));
    g.setColour (amber);
    g.drawText (TRANS ("bass") + " " + juce::String (rtBass, 2) + " s", line.removeFromRight (100), juce::Justification::centredRight, false);
    g.setColour (sky);
    g.drawText ("10 kHz " + juce::String (rtHigh, 2) + " s", line.removeFromRight (110), juce::Justification::centredRight, false);
    g.setColour (teal);
    g.drawText (TRANS ("early refl."), line.removeFromRight (90), juce::Justification::centredRight, false);

    // Panels
    fxui::drawSection (g, roomArea, TRANS ("Room"), teal);
    fxui::drawSection (g, decayArea, TRANS ("Decay"), lilac);
    fxui::drawSection (g, filterArea, TRANS ("Filters"), sky);
    fxui::drawSection (g, modArea, TRANS ("Modulation"), mint);
    fxui::drawSection (g, mixArea, TRANS ("Mix"), pink);
}
