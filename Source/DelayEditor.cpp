#include "DelayEditor.h"

using namespace theme::colours;

static constexpr int knobW = 76, knobH = 82, titleH = 24, choiceH = 24, pad = 10, gap = 8;

DelayEditor::DelayEditor (DelayPlugin& p) : plugin (p)
{
    modelChoice  = std::make_unique<fxui::ParamChoice> (*plugin.model, juce::StringArray { TRANS ("Mono"), TRANS ("Stereo"), TRANS ("Ping pong"), TRANS ("Off") }, teal);
    filterChoice = std::make_unique<fxui::ParamChoice> (*plugin.filterType, juce::StringArray { "Off", "LP", "HP", "BP" }, sky);
    distChoice   = std::make_unique<fxui::ParamChoice> (*plugin.distType, juce::StringArray { TRANS ("Limit"), TRANS ("Sat") }, orange);
    syncToggle   = std::make_unique<fxui::ParamToggle> (*plugin.sync, TRANS ("Tempo sync"), teal);
    pitchToggle  = std::make_unique<fxui::ParamToggle> (*plugin.keepPitch, TRANS ("Keep pitch"), teal);

    syncToggle->setTooltip (TRANS ("Echo times in note values that follow the song's tempo"));
    pitchToggle->setTooltip (TRANS ("Keep pitch: a new time crossfades instead of bending the echoes like tape"));

    for (juce::Component* c : { (juce::Component*) modelChoice.get(), (juce::Component*) filterChoice.get(), (juce::Component*) distChoice.get(),
                                (juce::Component*) syncToggle.get(), (juce::Component*) pitchToggle.get() })
        addAndMakeVisible (c);

    // Time
    add (plugin.input, TRANS ("Input"), teal);
    timeKnob = add (plugin.timeMs, TRANS ("Time"), teal);
    divisionKnob = add (plugin.division, TRANS ("Time"), teal);
    add (plugin.smoothing, TRANS ("Smooth"), teal);
    add (plugin.offset, TRANS ("Offset"), teal, true)->setTooltip (TRANS ("Mono / stereo: one side echoes later. Ping pong: where the input starts (pan)"));
    add (plugin.spread, TRANS ("Spread"), teal);

    // Feedback
    add (plugin.feedback, TRANS ("Level"), sky)->setTooltip (TRANS ("Feedback: how much of each echo comes back (over 100% grows)"));
    add (plugin.cutoff, TRANS ("Cutoff"), sky);
    add (plugin.resonance, TRANS ("Res"), sky);
    add (plugin.sampleRate, TRANS ("Samp rate"), sky);
    add (plugin.bits, TRANS ("Bits"), sky);

    // Modulation, diffusion, distortion, output
    add (plugin.modRate, TRANS ("Rate"), violet);
    add (plugin.modTime, TRANS ("Time"), violet);
    add (plugin.modCutoff, TRANS ("Cutoff"), violet);
    add (plugin.diffusion, TRANS ("Level"), mint);
    add (plugin.diffusionSpread, TRANS ("Spread"), mint);
    add (plugin.distKnee, TRANS ("Knee"), orange);
    add (plugin.distSymmetry, TRANS ("Symm"), orange);
    add (plugin.distLevel, TRANS ("Level"), orange);
    add (plugin.wet, TRANS ("Wet"), pink);
    add (plugin.tone, TRANS ("Tone"), pink, true)->setTooltip (TRANS ("Tone: left = darker (low pass), right = thinner (high pass)"));
    add (plugin.dry, TRANS ("Dry"), pink);

    syncToggle->onChange = [this] { updateTimeKnob(); };
    updateTimeKnob();

    const int width = 2 * 12 + (3 + 2 + 3 + 3) * knobW + 4 * 2 * pad + 3 * gap;
    setSize (width, 12 + 150 + 10 + 2 * (titleH + choiceH + knobH + 12) + gap + 12);
    startTimerHz (15);
}

fxui::ParamKnob* DelayEditor::add (te::AutomatableParameter* p, const juce::String& label, juce::Colour c, bool bipolar)
{
    auto* k = knobs.add (new fxui::ParamKnob (*p, label, c, bipolar));
    addAndMakeVisible (k);
    return k;
}

void DelayEditor::updateTimeKnob()
{
    const bool synced = plugin.sync->getCurrentValue() >= 0.5f;
    timeKnob->setVisible (! synced);
    divisionKnob->setVisible (synced);
    repaint();
}

void DelayEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    display = r.removeFromTop (150);
    r.removeFromTop (10);

    const int rowH = titleH + choiceH + knobH + 12;
    auto rowA = r.removeFromTop (rowH);
    r.removeFromTop (gap);
    auto rowB = r.removeFromTop (rowH);

    timeArea = rowA.removeFromLeft ((rowA.getWidth() - gap) / 2);
    rowA.removeFromLeft (gap);
    fbArea = rowA;

    auto take = [&rowB] (int knobsWide) { auto a = rowB.removeFromLeft (knobsWide * knobW + 2 * pad); rowB.removeFromLeft (gap); return a; };
    modArea = take (3);
    diffArea = take (2);
    distArea = take (3);
    outArea = rowB;

    auto knobRow = [] (juce::Rectangle<int> area) { return area.withTrimmedTop (titleH + choiceH + 4).reduced (pad, 0).withHeight (knobH); };
    auto choiceRow = [] (juce::Rectangle<int> area) { return area.withTrimmedTop (titleH).reduced (pad, 0).withHeight (choiceH - 2); };

    // Time panel: model buttons + sync / keep pitch, then Input, Time, Smooth, Offset, Spread.
    {
        auto c = choiceRow (timeArea);
        modelChoice->setBounds (c.removeFromLeft (c.getWidth() * 3 / 5));
        c.removeFromLeft (8);
        syncToggle->setBounds (c.removeFromLeft ((c.getWidth() - 4) / 2));
        c.removeFromLeft (4);
        pitchToggle->setBounds (c);

        auto k = knobRow (timeArea);
        const int w = k.getWidth() / 5;
        knobs[0]->setBounds (k.removeFromLeft (w));
        auto timeCell = k.removeFromLeft (w);
        knobs[1]->setBounds (timeCell);
        knobs[2]->setBounds (timeCell);
        for (int i = 3; i < 6; ++i) knobs[i]->setBounds (k.removeFromLeft (w));
    }

    // Feedback panel: filter type, then Level, Cutoff, Res, Samp rate, Bits.
    {
        filterChoice->setBounds (choiceRow (fbArea).removeFromLeft (220));
        auto k = knobRow (fbArea);
        const int w = k.getWidth() / 5;
        for (int i = 6; i < 11; ++i) knobs[i]->setBounds (k.removeFromLeft (w));
    }

    auto place = [&] (juce::Rectangle<int> area, int first, int count)
    {
        auto k = knobRow (area);
        const int w = k.getWidth() / count;
        for (int i = 0; i < count; ++i) knobs[first + i]->setBounds (k.removeFromLeft (w));
    };

    place (modArea, 11, 3);
    place (diffArea, 14, 2);
    distChoice->setBounds (choiceRow (distArea));
    place (distArea, 16, 3);
    place (outArea, 19, 3);
}

juce::String DelayEditor::stateKey() const
{
    juce::String k;

    for (auto* p : plugin.params())
        k << p->getCurrentValue() << ";";

    return k << plugin.getBpm();
}

void DelayEditor::timerCallback()
{
    const auto key = stateKey();

    if (key != shownKey)
    {
        shownKey = key;
        repaint (display);
    }
}

void DelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    // --- Echo map ---
    const auto s = plugin.getSettings();
    auto d = display.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (d, 6.0f);

    auto plot = display.reduced (14, 10).withTrimmedTop (18).toFloat();
    const float laneH = (plot.getHeight() - 8.0f) / 2.0f;
    auto laneL = plot.withHeight (laneH);
    auto laneR = plot.withTrimmedTop (laneH + 8.0f);
    const double echo = s.seconds();

    // Show about eight repeats (at least 1 s, at most 5 s).
    const double span = juce::jlimit (1.0, dly::maxSeconds, echo * (8.5 + (s.model == dly::mono || s.model == dly::stereo ? std::abs (s.offset) : 0.0)));
    auto xFor = [&plot, span] (double t) { return plot.getX() + (float) (t / span) * plot.getWidth(); };

    // Grid: beats when synced (one line per beat), else every 100 ms.
    const double gridStep = s.tempoSync ? 60.0 / juce::jmax (10.0, s.bpm) : 0.1;

    for (double t = 0.0; t <= span + 1.0e-9; t += gridStep)
    {
        g.setColour (grid);
        g.drawVerticalLine ((int) xFor (t), plot.getY(), plot.getBottom());
    }

    for (auto* lane : { &laneL, &laneR })
    {
        g.setColour (gridBar);
        g.drawHorizontalLine ((int) lane->getBottom(), plot.getX(), plot.getRight());
    }

    g.setFont (theme::ui (10.0f, theme::Weight::bold));
    g.setColour (dim);
    g.drawText ("L", (int) plot.getX() - 12, (int) laneL.getCentreY() - 6, 10, 12, juce::Justification::centred, false);
    g.drawText ("R", (int) plot.getX() - 12, (int) laneR.getCentreY() - 6, 10, 12, juce::Justification::centred, false);

    // The dry sound at 0
    auto bar = [&g] (juce::Rectangle<float> lane, float x, double level, juce::Colour c)
    {
        const float db = juce::Decibels::gainToDecibels ((float) level, -60.0f);
        const float h = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f) * lane.getHeight();

        if (h < 1.0f)
            return;

        g.setColour (c);
        g.fillRoundedRectangle (x - 2.0f, lane.getBottom() - h, 4.0f, h, 1.5f);
    };

    if (s.dry > 0.0)
    {
        bar (laneL, xFor (0.0), s.dry, text.withAlpha (0.8f));
        bar (laneR, xFor (0.0), s.dry, text.withAlpha (0.8f));
    }

    // The echoes (a mono / centred source, filter left out)
    if (s.model != dly::off && s.wet > 0.0)
    {
        const double fb = juce::jlimit (0.0, 1.2, s.feedback);
        // Mono / stereo: Offset delays one side's echoes (pre-delay); ping pong: Offset pans the input.
        const bool useOffset = s.model == dly::mono || s.model == dly::stereo;
        const double preL = useOffset ? echo * juce::jmax (0.0, -s.offset) : 0.0;
        const double preR = useOffset ? echo * juce::jmax (0.0, s.offset) : 0.0;
        const double gl = s.model == dly::pingPong ? juce::jmin (1.0, 1.0 - s.offset) : 1.0;
        const double gr = s.model == dly::pingPong ? juce::jmin (1.0, 1.0 + s.offset) : 1.0;
        double l = s.inputLevel * gl, r = s.inputLevel * gr;   // what goes into each side

        for (int n = 1; n <= 40; ++n)
        {
            const double tl = preL + echo * n, tr = preR + echo * n;

            if (tl > span && tr > span)
                break;

            const double levelL = juce::jmin (1.0, l) * s.wet, levelR = juce::jmin (1.0, r) * s.wet;
            const float fade = (float) juce::jlimit (0.25, 1.0, 1.0 - 0.06 * n);

            if (tl <= span) bar (laneL, xFor (tl), levelL, teal.withAlpha (fade));
            if (tr <= span) bar (laneR, xFor (tr), levelR, pink.withAlpha (fade));

            if (s.model == dly::pingPong)
            {
                const double nl = r * fb, nr = l * fb;   // the sides swap
                l = nl;
                r = nr;
            }
            else
            {
                l *= fb;
                r *= fb;
            }

            if (l < 1.0e-3 && r < 1.0e-3)
                break;
        }
    }

    // Read-out
    g.setFont (theme::mono (11.5f, true));
    g.setColour (text);
    juce::String readout;

    if (s.model == dly::off)
        readout = TRANS ("Off: no echoes (the repeat chain works as a filter / lo-fi / distortion)");
    else if (s.tempoSync)
        readout = juce::String (dly::divisions[juce::jlimit (0, dly::numDivisions - 1, s.division)].name) + " = "
                  + juce::String (juce::roundToInt (echo * 1000.0)) + " ms  @ " + juce::String (s.bpm, 1) + " BPM";
    else
        readout = juce::String (juce::roundToInt (echo * 1000.0)) + " ms";

    readout << "    " << TRANS ("feedback") << " " << juce::String (juce::roundToInt (s.feedback * 100.0)) << "%";
    g.drawText (readout, display.reduced (14, 6).removeFromTop (16), juce::Justification::centredLeft, false);

    if (s.feedback > 1.0)
    {
        g.setColour (red);
        g.drawText (TRANS ("feedback over 100%: the echoes grow"), display.reduced (14, 6).removeFromTop (16),
                    juce::Justification::centredRight, false);
    }

    // --- Panels ---
    fxui::drawSection (g, timeArea, TRANS ("Time"), teal);
    fxui::drawSection (g, fbArea, TRANS ("Feedback"), sky);
    fxui::drawSection (g, modArea, TRANS ("Modulation"), violet);
    fxui::drawSection (g, diffArea, TRANS ("Diffusion"), mint);
    fxui::drawSection (g, distArea, TRANS ("Distortion"), orange);
    fxui::drawSection (g, outArea, TRANS ("Output"), pink);
}
