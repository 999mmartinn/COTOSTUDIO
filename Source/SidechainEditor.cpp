#include "SidechainEditor.h"
#include "SongModel.h"

using namespace theme::colours;

static constexpr int knobW = 82, knobH = 82, pad = 12;

static juce::String describeTrack (te::Edit& edit, te::AudioTrack& t)
{
    if (const int n = song::getInsertNumber (edit, t); n > 0)
        return TRANS ("Insert") + " " + juce::String (n) + ": " + t.getName();

    return t.getName();
}

SidechainEditor::SidechainEditor (SidechainPlugin& p) : plugin (p)
{
    sourceButton.setTooltip (TRANS ("The sound that makes this one duck (usually the kick)"));
    sourceButton.onClick = [this] { chooseSource(); };
    addAndMakeVisible (sourceButton);

    auto add = [this] (te::AutomatableParameter* param, const juce::String& label, juce::Colour c, const juce::String& tip)
    {
        auto* k = knobs.add (new fxui::ParamKnob (*param, label, c));
        k->setTooltip (tip);
        addAndMakeVisible (k);
    };

    add (plugin.amount, TRANS ("Amount"), orange, TRANS ("How much quieter this sound gets at each hit (100% = silent for a moment)"));
    add (plugin.sensitivity, TRANS ("Sensitivity"), amber,
         TRANS ("How loud a hit must be to count. Lower = quieter hits make it dip too"));
    add (plugin.attack, TRANS ("Attack"), teal, TRANS ("How fast it dips when a hit comes"));
    add (plugin.release, TRANS ("Release"), teal,
         TRANS ("How long the sound takes to come all the way back after a hit. Longer = a slower, more \"pumping\" swell"));

    const juce::StringArray presets { TRANS ("Gentle"), TRANS ("Pumping"), TRANS ("Hard") };
    const juce::StringArray tips { TRANS ("A light dip: the kick gets a little room, you barely notice it"),
                                   TRANS ("The classic dance-music pump: the sound swells back after every kick"),
                                   TRANS ("Almost silent at each hit, back fast: for 808s and bass under a kick") };

    for (int i = 0; i < presets.size(); ++i)
    {
        auto* b = presetButtons.add (new juce::TextButton (presets[i]));
        b->setTooltip (tips[i]);
        b->onClick = [this, i] { plugin.applyPreset (i); };
        addAndMakeVisible (b);
    }

    gainHistory.fill (1.0f);   // (full volume until something plays)
    updateSourceButton();
    setSize (2 * pad + 4 * knobW + 3 * 10 + 2 * pad, 486);
    startTimerHz (30);
}

void SidechainEditor::updateSourceButton()
{
    auto* source = song::getSidechainSource (plugin.edit, plugin);
    const auto text = source != nullptr ? describeTrack (plugin.edit, *source) : TRANS ("Choose what it listens to...");

    if (text != shownSource)
    {
        shownSource = text;
        sourceButton.setButtonText (text);
        sourceButton.setColour (juce::TextButton::buttonColourId, source != nullptr ? accentSoft : redSoft);
        repaint();
    }
}

void SidechainEditor::chooseSource()
{
    auto& edit = plugin.edit;
    auto sources = song::getSidechainSources (edit, plugin);
    auto* current = song::getSidechainSource (edit, plugin);

    juce::PopupMenu m;
    m.addItem (1, TRANS ("Nothing (don't duck)"), true, current == nullptr);

    // Channels first (a kick channel is the usual pick), then the mixer inserts.
    for (int pass = 0; pass < 2; ++pass)
    {
        bool header = false;

        for (int i = 0; i < sources.size(); ++i)
        {
            auto* t = sources[i];

            if ((song::getInsertNumber (edit, *t) > 0) != (pass == 1))
                continue;

            if (! header)
                m.addSectionHeader (pass == 0 ? TRANS ("Channels") : TRANS ("Mixer inserts"));

            header = true;
            m.addItem (100 + i, describeTrack (edit, *t), true, t == current);
        }
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sourceButton),
                     [safe = juce::Component::SafePointer<SidechainEditor> (this), sources] (int r)
    {
        if (safe == nullptr || r <= 0)
            return;

        auto& e = safe->plugin.edit;
        te::AudioTrack* chosen = nullptr;

        if (r >= 100)
        {
            chosen = sources[r - 100];

            // (Still there? It may have been deleted while the menu was open.)
            if (! song::getSidechainSources (e, safe->plugin).contains (chosen))
                return;
        }

        e.getUndoManager().beginNewTransaction();
        song::setSidechainSource (e, safe->plugin, chosen);
        safe->updateSourceButton();
    });
}

void SidechainEditor::timerCallback()
{
    updateSourceButton();

    triggerHistory[(size_t) historyPos] = plugin.triggerPeak.exchange (0.0f);
    gainHistory[(size_t) historyPos] = plugin.lowestGain.exchange (1.0f);
    historyPos = (historyPos + 1) % historySize;
    repaint (display);
}

void SidechainEditor::resized()
{
    auto r = getLocalBounds().reduced (pad);
    sourceArea = r.removeFromTop (58);
    r.removeFromTop (10);
    display = r.removeFromTop (150);
    r.removeFromTop (10);
    knobArea = r.removeFromTop (knobH + 34);
    r.removeFromTop (10);
    presetArea = r.removeFromTop (58);
    helpArea = r;

    sourceButton.setBounds (sourceArea.withTrimmedTop (26).reduced (pad, 0).withHeight (26));

    auto k = knobArea.withTrimmedTop (28).reduced (pad, 0).withHeight (knobH);
    const int w = k.getWidth() / knobs.size();

    for (auto* knob : knobs)
        knob->setBounds (k.removeFromLeft (w).withSizeKeepingCentre (knobW, knobH));

    auto b = presetArea.withTrimmedTop (26).reduced (pad, 0).withHeight (24);
    const int bw = (b.getWidth() - 2 * 8) / 3;

    for (auto* button : presetButtons)
    {
        button->setBounds (b.removeFromLeft (bw));
        b.removeFromLeft (8);
    }
}

void SidechainEditor::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    fxui::drawSection (g, sourceArea, TRANS ("Listen to"), orange);
    fxui::drawSection (g, knobArea, TRANS ("How it ducks"), teal);
    fxui::drawSection (g, presetArea, TRANS ("Ready-made"), amber);

    // --- The last few seconds: trigger hits (bottom) and how loud this sound is let through (top) ---
    auto d = display.toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (d, 6.0f);

    auto plot = display.reduced (14, 10).withTrimmedTop (16).toFloat();
    const float triggerH = plot.getHeight() * 0.35f;
    auto gainLane = plot.withTrimmedBottom (triggerH + 8.0f);
    auto trigLane = plot.withTrimmedTop (plot.getHeight() - triggerH);

    g.setFont (theme::ui (10.0f, theme::Weight::bold));
    g.setColour (lcdMuted);
    g.drawText (fxui::upper (TRANS ("This sound")), display.reduced (14, 4).withHeight (14), juce::Justification::centredLeft, false);
    g.drawText (fxui::upper (TRANS ("Trigger")), trigLane.toNearestInt().translated (0, -14).withHeight (14),
                juce::Justification::centredLeft, false);

    g.setColour (gridBar);
    g.drawHorizontalLine ((int) gainLane.getY(), gainLane.getX(), gainLane.getRight());
    g.drawHorizontalLine ((int) trigLane.getBottom(), trigLane.getX(), trigLane.getRight());

    // The sensitivity, as a line across the trigger lane (-60..0 dB).
    auto yForDb = [&trigLane] (float db) { return trigLane.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f) * trigLane.getHeight(); };
    const float sensY = yForDb (plugin.sensitivity->getCurrentValue());
    g.setColour (amber.withAlpha (0.6f));
    g.drawHorizontalLine ((int) sensY, trigLane.getX(), trigLane.getRight());

    const float step = plot.getWidth() / (float) (historySize - 1);
    juce::Path gainPath;

    for (int i = 0; i < historySize; ++i)
    {
        const auto idx = (size_t) ((historyPos + i) % historySize);
        const float x = plot.getX() + (float) i * step;

        const float db = juce::Decibels::gainToDecibels (triggerHistory[idx], -60.0f);
        const float y = yForDb (db);

        if (y < trigLane.getBottom() - 0.5f)
        {
            g.setColour (y <= sensY ? orange : dim);
            g.fillRect (x - 1.0f, y, juce::jmax (2.0f, step - 1.0f), trigLane.getBottom() - y);
        }

        const float gy = gainLane.getBottom() - gainHistory[idx] * gainLane.getHeight();

        if (i == 0)
            gainPath.startNewSubPath (x, gy);
        else
            gainPath.lineTo (x, gy);
    }

    g.setColour (teal);
    g.strokePath (gainPath, juce::PathStrokeType (2.0f));

    // How much it ducks right now
    const float now = gainHistory[(size_t) ((historyPos + historySize - 1) % historySize)];
    g.setColour (lcdText);
    g.setFont (theme::ui (12.0f, theme::Weight::bold));
    const auto nowText = song::getSidechainSource (plugin.edit, plugin) == nullptr
                            ? TRANS ("not listening to anything")
                            : (now > 0.999f ? TRANS ("full volume")
                                            : juce::String (juce::Decibels::gainToDecibels (now, -60.0f), 1) + " dB");
    g.drawText (nowText, display.reduced (14, 4).withHeight (14), juce::Justification::centredRight, false);

    // Help
    g.setColour (muted);
    g.setFont (theme::ui (11.5f));
    g.drawFittedText (TRANS ("Put this on the sound that should make room (bass, pads, the music). "
                             "Pick what it listens to - usually the kick. Every time the kick hits, "
                             "this sound dips and comes back."),
                      helpArea.reduced (pad, 0), juce::Justification::topLeft, 4);
}
