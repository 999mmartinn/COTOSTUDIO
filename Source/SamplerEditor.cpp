#include "SamplerEditor.h"
#include "SongModel.h"

using namespace theme::colours;

static constexpr int knobW = 68, knobH = 82, titleH = 24, choiceH = 24, pad = 10, gap = 8;
static constexpr int waveH = 150, headerH = 26, graphW = 150, filterW = 220;

SamplerEditor::SamplerEditor (SamplerPlugin& p) : plugin (p)
{
    // Sample
    add (plugin.startParam, TRANS ("Start"), amber, TRANS ("Where the sound starts (or drag the S marker on the waveform)"));
    add (plugin.endParam, TRANS ("End"), amber, TRANS ("Where the sound ends (or drag the E marker)"));
    add (plugin.fadeInParam, TRANS ("Fade in"), amber, TRANS ("Softens the start of the sound (or drag the left dot)"));
    add (plugin.fadeOutParam, TRANS ("Fade out"), amber, TRANS ("Softens the end of the sound - handy after trimming an 808 (or drag the right dot)"));

    // Pitch and level
    add (plugin.tuneParam, TRANS ("Tune"), lilac, TRANS ("Pitch in semitones (the piano roll's notes shift it too: C5 plays it as recorded)"));
    add (plugin.fineParam, TRANS ("Fine"), lilac, TRANS ("Fine pitch in cents (100 = a semitone)"));
    add (plugin.levelParam, TRANS ("Level"), lilac, TRANS ("This sound's volume (the rack's knob and the mixer come after it)"));

    // Envelope
    add (plugin.attackParam, TRANS ("Attack"), teal, TRANS ("Time to rise to full volume when a note starts"));
    add (plugin.holdParam, TRANS ("Hold"), teal, TRANS ("Time it stays at full volume"));
    add (plugin.decayParam, TRANS ("Decay"), teal, TRANS ("Time to fall to the sustain level"));
    add (plugin.sustainParam, TRANS ("Sustain"), teal, TRANS ("Level kept while the note lasts"));
    add (plugin.releaseParam, TRANS ("Release"), teal, TRANS ("Time to fade out after the note ends"));

    // Filter
    add (plugin.cutoffParam, TRANS ("Cutoff"), sky, TRANS ("Where the filter starts working"));
    add (plugin.resonanceParam, TRANS ("Res"), sky, TRANS ("Resonance: a boost at the cutoff (high = a whistling peak)"));

    reverseToggle = std::make_unique<fxui::ParamToggle> (*plugin.reverseParam, TRANS ("Reverse"), amber);
    reverseToggle->setTooltip (TRANS ("Plays the sound backwards (the trim and the fades follow the reversed sound)"));
    envelopeToggle = std::make_unique<fxui::ParamToggle> (*plugin.envOnParam, TRANS ("On"), teal);
    envelopeToggle->setTooltip (TRANS ("On: attack, hold, decay and sustain shape the sound, and it fades out (release) when the note ends. "
                                       "Off: the whole sound plays, however short the note (one-shot)"));
    envelopeToggle->onChange = [this]
    {
        envelopeToggle->setButtonText (envelopeToggle->isOn() ? TRANS ("On") : TRANS ("Off"));
        repaint (envelopeGraph);
    };
    envelopeToggle->onChange();
    filterChoice = std::make_unique<fxui::ParamChoice> (*plugin.filterTypeParam,
                                                        juce::StringArray { "Off", "LP", "HP", "BP" }, sky);
    filterChoice->setTooltip (TRANS ("Low pass (LP) keeps the lows: darker. High pass (HP) keeps the highs: thinner. Band pass (BP) keeps the middle"));

    cutButton.setButtonText (TRANS ("Cut itself"));
    cutButton.setTooltip (TRANS ("Cut itself: a new hit stops the last one (for 808s). Also in the channel's right-click menu"));
    cutButton.onClick = [this]
    {
        plugin.edit.getUndoManager().beginNewTransaction();
        plugin.cutValue = ! plugin.cutValue.get();
        refreshCutButton();
    };
    noteLengthButton.setButtonText (TRANS ("Note length"));
    noteLengthButton.setTooltip (TRANS ("Stop at note end: the sound plays only as long as its note (shorten the note, shorten the 808). "
                                        "Off: the whole sound plays however short the note is. Also in the channel's right-click menu"));
    noteLengthButton.onClick = [this]
    {
        plugin.edit.getUndoManager().beginNewTransaction();
        plugin.noteLengthValue = ! plugin.noteLengthValue.get();
        refreshCutButton();
    };
    refreshCutButton();

    for (auto* c : std::initializer_list<juce::Component*> { reverseToggle.get(), envelopeToggle.get(), filterChoice.get(), &cutButton, &noteLengthButton })
        addAndMakeVisible (c);

    const int width = 2 * 12 + (pad + graphW + 8 + 5 * knobW + pad) + gap + filterW;
    const int rowH = titleH + choiceH + 4 + knobH + 12;
    setSize (width, 12 + headerH + 6 + waveH + 10 + 2 * rowH + gap + 12);
    startTimerHz (30);
}

fxui::ParamKnob* SamplerEditor::add (te::AutomatableParameter* p, const juce::String& label, juce::Colour c, const juce::String& tip)
{
    auto* k = knobs.add (new fxui::ParamKnob (*p, label, c));
    k->setTooltip (tip);
    addAndMakeVisible (k);
    return k;
}

void SamplerEditor::refreshCutButton()
{
    cutShown = plugin.cutValue.get();
    cutButton.setColour (juce::TextButton::buttonColourId, cutShown ? amber : well);
    cutButton.setColour (juce::TextButton::textColourOffId, cutShown ? accentText : muted);
    noteLengthShown = plugin.noteLengthValue.get();
    noteLengthButton.setColour (juce::TextButton::buttonColourId, noteLengthShown ? amber : well);
    noteLengthButton.setColour (juce::TextButton::textColourOffId, noteLengthShown ? accentText : muted);
}

void SamplerEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    header = r.removeFromTop (headerH);
    r.removeFromTop (6);
    wave = r.removeFromTop (waveH);
    r.removeFromTop (10);

    const int rowH = titleH + choiceH + 4 + knobH + 12;
    auto rowA = r.removeFromTop (rowH);
    r.removeFromTop (gap);
    auto rowB = r.removeFromTop (rowH);

    sampleArea = rowA.removeFromLeft ((rowA.getWidth() - gap) * 4 / 7);
    rowA.removeFromLeft (gap);
    pitchArea = rowA;

    filterArea = rowB.removeFromRight (filterW);
    rowB.removeFromRight (gap);
    envelopeArea = rowB;

    auto knobRow = [] (juce::Rectangle<int> area) { return area.withTrimmedTop (titleH + choiceH + 4).reduced (pad, 0).withHeight (knobH); };
    auto place = [&] (juce::Rectangle<int> k, std::initializer_list<int> which)
    {
        const int w = k.getWidth() / (int) which.size();
        for (auto i : which) knobs[i]->setBounds (k.removeFromLeft (w));
    };

    // Sample: Reverse, Cut itself and Note length on the button row.
    auto buttons = sampleArea.withTrimmedTop (titleH).reduced (pad, 0).withHeight (choiceH - 2);
    reverseToggle->setBounds (buttons.removeFromLeft (100));
    buttons.removeFromLeft (8);
    cutButton.setBounds (buttons.removeFromLeft (110));
    buttons.removeFromLeft (8);
    noteLengthButton.setBounds (buttons.removeFromLeft (110));
    place (knobRow (sampleArea), { 0, 1, 2, 3 });
    place (knobRow (pitchArea), { 4, 5, 6 });

    // Envelope: On in its title row, the shape on the left, then the knobs.
    envelopeToggle->setBounds (envelopeArea.withTrimmedTop (4).withHeight (titleH - 6).removeFromRight (92).translated (-pad, 0));
    envelopeGraph = envelopeArea.withTrimmedTop (titleH).reduced (pad, 0).withHeight (choiceH + 4 + knobH).removeFromLeft (graphW);
    place (knobRow (envelopeArea).withTrimmedLeft (graphW + 8), { 7, 8, 9, 10, 11 });

    filterChoice->setBounds (filterArea.withTrimmedTop (titleH).reduced (pad, 0).withHeight (choiceH - 2));
    place (knobRow (filterArea), { 12, 13 });

    renderWave();
}

//==============================================================================
juce::Rectangle<float> SamplerEditor::plotArea() const
{
    return wave.toFloat().reduced (12.0f, 22.0f).withTrimmedBottom (-6.0f);
}

float SamplerEditor::xForFraction (float f) const
{
    const auto p = plotArea();
    return p.getX() + juce::jlimit (0.0f, 1.0f, f) * p.getWidth();
}

float SamplerEditor::fractionAtX (float x) const
{
    const auto p = plotArea();
    return juce::jlimit (0.0f, 1.0f, (x - p.getX()) / juce::jmax (1.0f, p.getWidth()));
}

juce::String SamplerEditor::makeWaveKey() const
{
    juce::String key;
    key << plugin.getLoadedHash() << ";" << (int) plugin.getPeaks().size() << ";" << wave.toString();

    for (auto* p : { plugin.startParam, plugin.endParam, plugin.fadeInParam, plugin.fadeOutParam, plugin.reverseParam })
        key << ";" << p->getCurrentValue();

    return key;
}

/** The waveform, the trim and the fades into an image (only when they change: cheap on old GPUs). */
void SamplerEditor::renderWave()
{
    if (wave.isEmpty())
        return;

    waveKey = makeWaveKey();
    waveImage = juce::Image (juce::Image::ARGB, wave.getWidth(), wave.getHeight(), true);
    juce::Graphics g (waveImage);
    g.addTransform (juce::AffineTransform::translation ((float) -wave.getX(), (float) -wave.getY()));

    const auto box = wave.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (box, 6.0f);

    const auto plot = plotArea();
    const float mid = plot.getCentreY();
    const auto& peaks = plugin.getPeaks();
    const double seconds = plugin.getSampleSeconds();

    g.setColour (lcdMuted.withAlpha (0.25f));
    g.drawHorizontalLine ((int) mid, plot.getX(), plot.getRight());

    if (peaks.empty() || seconds <= 0.0)
    {
        g.setColour (lcdMuted);
        g.setFont (theme::ui (13.0f, theme::Weight::medium));
        g.drawText (plugin.getAsset().isEmpty() ? TRANS ("No sound yet: drop one on this channel in the rack")
                                                : TRANS ("The sound is still arriving..."),
                    wave, juce::Justification::centred, false);
        return;
    }

    const bool reverse = plugin.reverseParam->getCurrentValue() >= 0.5f;
    const float start = juce::jmin (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float end = juce::jmax (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float xs = xForFraction (start), xe = xForFraction (end);

    // Loudest point, so quiet sounds still show (the scale is written in the corner).
    float loudest = 0.0f;

    for (auto v : peaks)
        loudest = juce::jmax (loudest, std::abs (v));

    const float scale = (plot.getHeight() * 0.5f) / juce::jmax (0.05f, loudest);
    const int bins = (int) peaks.size() / 2;

    for (int px = (int) plot.getX(); px < (int) plot.getRight(); ++px)
    {
        const float f0 = fractionAtX ((float) px), f1 = fractionAtX ((float) px + 1.0f);
        int b0 = (int) (f0 * bins), b1 = juce::jmax (b0 + 1, (int) (f1 * bins));

        if (reverse)   // drawn in the order it plays
            std::tie (b0, b1) = std::make_pair (bins - b1, bins - b0);

        float lo = 0.0f, hi = 0.0f;

        for (int b = juce::jmax (0, b0); b < juce::jmin (bins, b1); ++b)
        {
            lo = juce::jmin (lo, peaks[(size_t) b * 2]);
            hi = juce::jmax (hi, peaks[(size_t) b * 2 + 1]);
        }

        const bool inside = px >= xs && px <= xe;
        g.setColour (inside ? lilac : lcdMuted.withAlpha (0.35f));
        g.drawVerticalLine (px, mid - hi * scale, mid - lo * scale + 1.0f);
    }

    // Outside the trim: shaded.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), box.getY() + 2.0f, xs, box.getBottom() - 2.0f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xe, box.getY() + 2.0f, plot.getRight(), box.getBottom() - 2.0f));

    // The volume line: up over the fade in, down over the fade out.
    const float fi = (float) (plugin.fadeInParam->getCurrentValue() * 0.001 / seconds);
    const float fo = (float) (plugin.fadeOutParam->getCurrentValue() * 0.001 / seconds);
    const float top = plot.getY() - 6.0f, bottom = plot.getBottom();
    const float xi = juce::jmin (xe, xForFraction (start + fi)), xo = juce::jmax (xs, xForFraction (end - fo));
    juce::Path gain;
    gain.startNewSubPath (xs, fi > 0.0f ? bottom : top);
    gain.lineTo (xi, top);
    gain.lineTo (juce::jmax (xi, xo), top);
    gain.lineTo (xe, fo > 0.0f ? bottom : top);
    g.setColour (amber.withAlpha (0.85f));
    g.strokePath (gain, juce::PathStrokeType (1.5f));

    for (auto x : { xi, juce::jmax (xi, xo) })
    {
        g.setColour (lcd);
        g.fillEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ x, top }));
        g.setColour (amber);
        g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ x, top }), 2.0f);
    }

    // Start and end markers with their flags.
    for (int i = 0; i < 2; ++i)
    {
        const float x = i == 0 ? xs : xe;
        g.setColour (amber);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, box.getY() + 4.0f, 2.0f, box.getHeight() - 8.0f));
        auto flag = i == 0 ? juce::Rectangle<float> (x, box.getY() + 4.0f, 16.0f, 14.0f)
                           : juce::Rectangle<float> (x - 16.0f, box.getBottom() - 18.0f, 16.0f, 14.0f);
        g.fillRoundedRectangle (flag, 3.0f);
        g.setColour (accentText);
        g.setFont (theme::mono (10.0f, true));
        g.drawText (i == 0 ? "S" : "E", flag, juce::Justification::centred, false);
    }

    // Times: where the trim starts and how long it plays (at the recorded pitch).
    g.setFont (theme::mono (10.5f));
    g.setColour (lcdMuted);
    const auto length = juce::String ((end - start) * seconds, 3) + " s";
    g.drawText ((reverse ? TRANS ("reversed") + "  " : juce::String()) + length + " / " + juce::String (seconds, 3) + " s",
                wave.reduced (12, 5).withTrimmedLeft (10), juce::Justification::bottomLeft, false);
}

void SamplerEditor::paintEnvelope (juce::Graphics& g)
{
    const auto box = envelopeGraph.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (box, 6.0f);

    const bool on = plugin.envOnParam->getCurrentValue() >= 0.5f;
    auto plot = box.reduced (10.0f, 12.0f);

    // Widths by the square root of each time, so short and long times both show.
    const float a = std::sqrt (plugin.attackParam->getCurrentValue()), h = std::sqrt (plugin.holdParam->getCurrentValue());
    const float d = std::sqrt (plugin.decayParam->getCurrentValue()), rel = std::sqrt (plugin.releaseParam->getCurrentValue());
    const float sus = 0.22f * (a + h + d + rel) + 8.0f;
    const float unit = plot.getWidth() / juce::jmax (1.0f, a + h + d + sus + rel);
    const float s = plugin.sustainParam->getCurrentValue();
    const float yTop = plot.getY(), yBottom = plot.getBottom(), yS = yBottom - s * plot.getHeight();

    juce::Path shape;
    float x = plot.getX();
    shape.startNewSubPath (x, yBottom);
    shape.lineTo (x += a * unit, yTop);
    shape.lineTo (x += h * unit, yTop);
    shape.lineTo (x += d * unit, yS);
    shape.lineTo (x += sus * unit, yS);
    shape.lineTo (x += rel * unit, yBottom);

    auto fill = shape;
    fill.closeSubPath();
    const auto colour = on ? teal : lcdMuted.withAlpha (0.45f);
    g.setColour (colour.withAlpha (on ? 0.16f : 0.08f));
    g.fillPath (fill);
    g.setColour (colour);
    g.strokePath (shape, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (! on)
    {
        // A label on a dark pill, so it reads over the shape.
        const auto label = envelopeGraph.reduced (10).withSizeKeepingCentre (envelopeGraph.getWidth() - 20, 38).toFloat();
        g.setColour (lcd.withAlpha (0.9f));
        g.fillRoundedRectangle (label, 5.0f);
        g.setColour (lcdMuted);
        g.setFont (theme::ui (10.5f, theme::Weight::semibold));
        g.drawFittedText (TRANS ("Off: one-shot (the whole sound plays)"), label.toNearestInt().reduced (4, 0), juce::Justification::centred, 2);
    }
}

void SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    // Header: the sound's name and length.
    auto h = header;
    theme::drawIcon (g, theme::Icon::wave, h.removeFromLeft (18).toFloat().withSizeKeepingCentre (16.0f, 16.0f), amber);
    h.removeFromLeft (8);
    const auto name = plugin.getAsset().isEmpty() ? TRANS ("No sound") : song::getAssetName (plugin.edit, plugin.getAsset());
    g.setColour (text);
    g.setFont (theme::ui (14.0f, theme::Weight::bold));
    const int nameW = juce::jmin (h.getWidth() - 120, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 4);
    g.drawText (name, h.removeFromLeft (nameW), juce::Justification::centredLeft, true);
    g.setColour (muted);
    g.setFont (theme::ui (11.5f));
    g.drawText (TRANS ("Edit sound"), h, juce::Justification::centredRight, false);

    if (waveImage.isValid())
        g.drawImageAt (waveImage, wave.getX(), wave.getY());

    // Where the hits are playing right now.
    g.setColour (text.withAlpha (0.9f));

    for (auto f : shownVoices)
        if (f >= 0.0f)
            g.fillRect (juce::Rectangle<float> (xForFraction (f) - 0.75f, (float) wave.getY() + 4.0f, 1.5f, (float) wave.getHeight() - 8.0f));

    fxui::drawSection (g, sampleArea, TRANS ("Sample"), amber);
    fxui::drawSection (g, pitchArea, TRANS ("Pitch & level"), lilac);
    fxui::drawSection (g, envelopeArea, TRANS ("Envelope"), teal);
    fxui::drawSection (g, filterArea, TRANS ("Filter"), sky);
    paintEnvelope (g);
}

//==============================================================================
SamplerEditor::Handle SamplerEditor::handleAt (juce::Point<float> p) const
{
    const double seconds = plugin.getSampleSeconds();

    if (! wave.contains (p.toInt()) || seconds <= 0.0)
        return Handle::none;

    const float start = juce::jmin (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float end = juce::jmax (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float top = plotArea().getY() - 6.0f;
    const float xs = xForFraction (start), xe = xForFraction (end);
    const float xi = juce::jmin (xe, xForFraction (start + (float) (plugin.fadeInParam->getCurrentValue() * 0.001 / seconds)));
    const float xo = juce::jmax (xi, juce::jmax (xs, xForFraction (end - (float) (plugin.fadeOutParam->getCurrentValue() * 0.001 / seconds))));

    if (p.getDistanceFrom ({ xo, top }) < 8.0f)  return Handle::fadeOut;
    if (p.getDistanceFrom ({ xi, top }) < 8.0f)  return Handle::fadeIn;
    if (std::abs (p.x - xe) < 6.0f)              return Handle::end;
    if (std::abs (p.x - xs) < 6.0f)              return Handle::start;
    return Handle::none;
}

void SamplerEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto h = handleAt (e.position);
    setMouseCursor (h == Handle::start || h == Handle::end ? juce::MouseCursor::LeftRightResizeCursor
                    : h != Handle::none ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void SamplerEditor::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleAt (e.position);

    if (dragging != Handle::none)
        plugin.edit.getUndoManager().beginNewTransaction();
}

void SamplerEditor::mouseDrag (const juce::MouseEvent& e)
{
    const double seconds = plugin.getSampleSeconds();

    if (dragging == Handle::none || seconds <= 0.0)
        return;

    const float f = fractionAtX (e.position.x);
    const float start = juce::jmin (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float end = juce::jmax (plugin.startParam->getCurrentValue(), plugin.endParam->getCurrentValue());
    const float tiny = (float) juce::jmin (0.5, 0.002 / seconds);   // keep at least 2 ms

    switch (dragging)
    {
        case Handle::start:   plugin.startParam->setParameter (juce::jlimit (0.0f, end - tiny, f), juce::sendNotification); break;
        case Handle::end:     plugin.endParam->setParameter (juce::jlimit (start + tiny, 1.0f, f), juce::sendNotification); break;
        case Handle::fadeIn:  plugin.fadeInParam->setParameter (juce::jlimit (0.0f, 2000.0f, (float) ((f - start) * seconds * 1000.0)), juce::sendNotification); break;
        case Handle::fadeOut: plugin.fadeOutParam->setParameter (juce::jlimit (0.0f, 2000.0f, (float) ((end - f) * seconds * 1000.0)), juce::sendNotification); break;
        case Handle::none:
        default: break;
    }

    timerCallback();
}

void SamplerEditor::mouseUp (const juce::MouseEvent&)
{
    dragging = Handle::none;
}

void SamplerEditor::timerCallback()
{
    if (makeWaveKey() != waveKey)
    {
        renderWave();
        repaint (wave.expanded (2));
        repaint (header);
    }

    juce::String key;

    for (auto* p : { plugin.envOnParam, plugin.attackParam, plugin.holdParam, plugin.decayParam, plugin.sustainParam, plugin.releaseParam })
        key << p->getCurrentValue() << ";";

    if (key != envelopeKey)
    {
        envelopeKey = key;
        repaint (envelopeGraph);
    }

    if (plugin.cutValue.get() != cutShown || plugin.noteLengthValue.get() != noteLengthShown)
    {
        refreshCutButton();
        cutButton.repaint();
        noteLengthButton.repaint();
    }

    // The play lines (only repaint while something plays or just stopped).
    bool changed = false;

    for (int i = 0; i < SamplerPlugin::maxVoices; ++i)
    {
        const float f = plugin.getVoicePosition (i);

        if (std::abs (f - shownVoices[(size_t) i]) > 1.0e-5f)
        {
            shownVoices[(size_t) i] = f;
            changed = true;
        }
    }

    if (changed)
        repaint (wave);
}
