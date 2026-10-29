#include "LimiterEditor.h"

using namespace theme::colours;

static constexpr int knobW = 76, knobH = 82, sectionTop = 24, pad = 10;

LimiterEditor::LimiterEditor (LimiterPlugin& p) : plugin (p)
{
    // Compressor (blue), limiter (green), gate & saturation (amber); gains in violet, like FL.
    add (plugin.compGain,    TRANS ("Gain"),    violet, true);
    add (plugin.compThresh,  TRANS ("Thresh"),  sky);
    add (plugin.compRatio,   TRANS ("Ratio"),   sky);
    add (plugin.compKnee,    TRANS ("Knee"),    sky);
    add (plugin.compAttack,  TRANS ("Attack"),  sky);
    add (plugin.compRelease, TRANS ("Release"), sky);
    add (plugin.compCurve,   TRANS ("Curve"),   sky);
    add (plugin.compSustain, TRANS ("Sustain"), sky);

    add (plugin.limGain,     TRANS ("Gain"),    violet, true);
    add (plugin.ceiling,     TRANS ("Ceiling"), mint);
    add (plugin.limAttack,   TRANS ("Attack"),  mint)->setTooltip (TRANS ("Attack = look-ahead: the limiter sees peaks this early (and adds this much latency, compensated by the mixer)"));
    add (plugin.limRelease,  TRANS ("Release"), mint);
    add (plugin.limCurve,    TRANS ("Curve"),   mint);
    add (plugin.limSustain,  TRANS ("Sustain"), mint)->setTooltip (TRANS ("Sustain: holds the gain down this long before releasing"));

    add (plugin.gateGain,    TRANS ("Gate"),    amber)->setTooltip (TRANS ("Noise gate: how much passes while it is closed (100% = no gating)"));
    add (plugin.gateThresh,  TRANS ("Thresh"),  amber);
    add (plugin.gateRelease, TRANS ("Release"), amber);
    add (plugin.saturation,  TRANS ("Sat"),     amber)->setTooltip (TRANS ("Saturation: soft clipping that starts this many dB below the ceiling"));

    plugin.displayWanted = true;
    setSize (2 * 12 + (4 + 3 + 2) * knobW + 3 * 2 * pad + 2 * 8, 410);
    startTimerHz (30);
}

LimiterEditor::~LimiterEditor()
{
    plugin.displayWanted = false;
}

fxui::ParamKnob* LimiterEditor::add (te::AutomatableParameter* p, const juce::String& label, juce::Colour c, bool bipolar)
{
    auto* k = knobs.add (new fxui::ParamKnob (*p, label, c, bipolar));
    addAndMakeVisible (k);
    return k;
}

float LimiterEditor::yForDb (float db) const
{
    auto r = display.toFloat().reduced (0.0f, 6.0f);
    const float t = (juce::jlimit (bottomDb, topDb, db) - topDb) / (bottomDb - topDb);
    return r.getY() + t * r.getHeight();
}

void LimiterEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    display = r.removeFromTop (172);
    r.removeFromTop (10);

    const int compW = 4 * knobW + 2 * pad, limW = 3 * knobW + 2 * pad, gateW = 2 * knobW + 2 * pad;
    compArea = r.removeFromLeft (compW);
    r.removeFromLeft (8);
    limArea = r.removeFromLeft (limW);
    r.removeFromLeft (8);
    gateArea = r.removeFromLeft (gateW);

    auto place = [this] (juce::Rectangle<int> area, int first, int count, int columns)
    {
        auto inner = area.withTrimmedTop (sectionTop).reduced (pad, 4);

        for (int i = 0; i < count; ++i)
            knobs[first + i]->setBounds (inner.getX() + (i % columns) * knobW, inner.getY() + (i / columns) * knobH, knobW, knobH);
    };

    place (compArea, 0, 8, 4);
    place (limArea, 8, 6, 3);
    place (gateArea, 14, 4, 2);

    history.assign ((size_t) juce::jmax (10, display.getWidth() - 2), {});
    writePos = 0;
}

void LimiterEditor::mouseDown (const juce::MouseEvent& e)
{
    if (display.contains (e.getPosition()))
    {
        paused = ! paused;
        repaint (display);
    }
}

void LimiterEditor::timerCallback()
{
    auto& fifo = plugin.frameFifo;
    const int ready = fifo.getNumReady();

    if (ready > 0)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (ready, s1, n1, s2, n2);

        auto take = [this] (const lim::Frame& f)
        {
            shownGr = f.gainDb;
            shownOut = f.outPeak;

            if (paused || history.empty())
                return;

            history[(size_t) writePos] = f;
            writePos = (writePos + 1) % (int) history.size();
        };

        for (int i = 0; i < n1; ++i) take (plugin.frames[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) take (plugin.frames[(size_t) (s2 + i)]);
        fifo.finishedRead (n1 + n2);
    }
    else if (! paused && ! history.empty())
    {
        // Nothing playing: keep scrolling with silence.
        history[(size_t) writePos] = {};
        writePos = (writePos + 1) % (int) history.size();
        shownGr = 0.0f;
        shownOut = 0.0f;
    }

    repaint (display);
}

void LimiterEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    // --- Display ---
    auto d = display.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (d, 6.0f);

    g.setFont (theme::mono (9.5f));

    for (float db : { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -30.0f, -36.0f })
    {
        g.setColour (db == 0.0f ? gridBar : grid);
        g.drawHorizontalLine ((int) yForDb (db), d.getX() + 28.0f, d.getRight() - 4.0f);
    }

    if (! history.empty())
    {
        const int n = (int) history.size();
        const float x0 = d.getRight() - (float) n;
        juce::Path inPath, outPath, gainPath;
        const float base = d.getBottom() - 6.0f;
        outPath.startNewSubPath (x0, base);

        for (int k = 0; k < n; ++k)
        {
            const auto& f = history[(size_t) ((writePos + k) % n)];
            const float x = x0 + (float) k;
            const float iy = yForDb (juce::Decibels::gainToDecibels (f.inPeak, -100.0f));

            if (k == 0) inPath.startNewSubPath (x, iy);
            else        inPath.lineTo (x, iy);

            outPath.lineTo (x, yForDb (juce::Decibels::gainToDecibels (f.outPeak, -100.0f)));

            // The gain envelope hangs from the top: 0 dB of reduction = the top line.
            const float gy = yForDb (topDb + f.gainDb);

            if (k == 0) gainPath.startNewSubPath (x, gy);
            else        gainPath.lineTo (x, gy);
        }

        outPath.lineTo (d.getRight(), base);
        outPath.closeSubPath();

        // Output peaks filled (green), input peaks as a line on top (violet), gain envelope (white).
        g.saveState();
        g.reduceClipRegion (display.reduced (1));
        g.setColour (mint.withAlpha (0.5f));
        g.fillPath (outPath);
        g.setColour (violet.withAlpha (0.95f));
        g.strokePath (inPath, juce::PathStrokeType (1.2f));
        g.setColour (text.withAlpha (0.9f));
        g.strokePath (gainPath, juce::PathStrokeType (1.4f));
        g.restoreState();
    }

    // Threshold (blue) and ceiling (green) lines
    auto marker = [&g, &d, this] (float db, juce::Colour c, const juce::String& label)
    {
        const float y = yForDb (db);
        g.setColour (c.withAlpha (0.9f));
        g.drawHorizontalLine ((int) y, d.getX() + 28.0f, d.getRight() - 4.0f);
        g.setFont (theme::ui (10.0f, theme::Weight::semibold));
        g.drawText (label, (int) d.getRight() - 90, (int) y - 13, 84, 12, juce::Justification::centredRight, false);
    };

    if (plugin.compThresh->getCurrentValue() < -0.05f)
        marker (plugin.compThresh->getCurrentValue(), sky, TRANS ("THRESH"));

    if (plugin.ceiling->getCurrentValue() < 11.95f)
        marker (plugin.ceiling->getCurrentValue(), mint, TRANS ("CEILING"));

    // dB scale on top of the levels
    g.setFont (theme::mono (9.5f));

    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f })
    {
        const float y = yForDb (db);
        g.setColour (lcd.withAlpha (0.8f));
        g.fillRect ((int) d.getX() + 4, (int) y - 6, 24, 12);
        g.setColour (dim);
        g.drawText (juce::String ((int) db), (int) d.getX() + 2, (int) y - 6, 24, 12, juce::Justification::centredRight, false);
    }

    // Read-out (on a dark backing so the levels never hide it)
    g.setColour (lcd.withAlpha (0.85f));
    g.fillRoundedRectangle (display.reduced (6).removeFromTop (20).removeFromLeft (230).toFloat(), 4.0f);
    g.fillRoundedRectangle (display.reduced (6).removeFromBottom (18).removeFromLeft (200).toFloat(), 4.0f);
    g.setFont (theme::mono (11.5f, true));
    g.setColour (text);
    const auto readout = "GR " + juce::String (shownGr, 1) + " dB    OUT "
                         + juce::String (juce::Decibels::gainToDecibels (shownOut, -100.0f), 1) + " dB";
    g.drawText (readout, display.reduced (10, 6).removeFromTop (16), juce::Justification::centredLeft, false);

    g.setFont (theme::ui (10.5f, theme::Weight::medium));
    g.setColour (muted);
    g.drawText (paused ? TRANS ("paused - click to go on")
                       : TRANS ("look-ahead") + " " + juce::String (plugin.getLatencySeconds() * 1000.0, 1) + " ms",
                display.reduced (10, 6).removeFromBottom (14), juce::Justification::centredLeft, false);

    // --- Panels ---
    fxui::drawSection (g, compArea, TRANS ("Compressor"), sky);
    fxui::drawSection (g, limArea, TRANS ("Limiter"), mint);
    fxui::drawSection (g, gateArea, TRANS ("Gate & saturation"), amber);
}
