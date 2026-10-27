#include "FlpImportDialog.h"
#include "Theme.h"

namespace col = theme::colours;

//==============================================================================
struct FlpImportDialog::Row  : public juce::Component
{
    Row (const flimport::Project& p, int n) : number (n)
    {
        auto& ins = p.inserts.getReference (n);

        tick.setButtonText (juce::String (n) + "   " + ins.displayName());
        tick.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (tick);

        juce::StringArray to;
        int closed = 0;

        for (auto& r : ins.routes)
        {
            if (r.closed)
                ++closed;
            else
                to.add (r.to == 0 ? TRANS ("Master") : p.inserts[r.to].displayName());
        }

        if (closed > 0)
            to.add (TRANS ("+{N} closed").replace ("{N}", juce::String (closed)));

        int vst = 0, fl = 0;
        for (auto& fx : ins.effects)
            (fx.isVST ? vst : fl)++;

        juce::String text;

        if (ins.effects.isEmpty())
            text = TRANS ("no effects");
        else
            text = TRANS ("{N} effect(s)").replace ("{N}", juce::String (ins.effects.size()))
                 + (fl > 0 ? " (" + TRANS ("{N} from FL") .replace ("{N}", juce::String (fl)) + ")" : juce::String());

        detail.setText (text + "   ->  " + (to.isEmpty() ? TRANS ("nowhere") : to.joinIntoString (", ")), juce::dontSendNotification);
        detail.setColour (juce::Label::textColourId, col::muted);
        detail.setFont (theme::ui (12.0f));
        detail.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (detail);

        juce::StringArray chain;
        for (auto& fx : ins.effects)
            chain.add (fx.describe() + (fx.enabled ? "" : " (" + TRANS ("off") + ")"));

        const auto tip = chain.joinIntoString ("\n");
        tick.setTooltip (tip);
        detail.setTooltip (tip);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 0);
        tick.setBounds (r.removeFromLeft (210));
        detail.setBounds (r);
    }

    const int number;
    juce::ToggleButton tick;
    juce::Label detail;
};

//==============================================================================
FlpImportDialog::FlpImportDialog (const flimport::Project& p, const juce::String& fileName,
                                  std::function<void (const flimport::Options&)> done)
    : project (p), onImport (std::move (done))
{
    auto& master = project.inserts.getReference (0);
    const auto used = project.getUsedInserts();
    int namedTracks = 0;

    for (auto& t : project.trackNames)
        namedTracks += t.isNotEmpty() ? 1 : 0;

    intro.setText (TRANS ("\"{FILE}\" (FL Studio {V}): {BPM} BPM, {N} mixer inserts in use, the master with {M} effect(s).")
                     .replace ("{FILE}", fileName).replace ("{V}", project.flVersion)
                     .replace ("{BPM}", juce::String (project.bpm, project.bpm == std::floor (project.bpm) ? 0 : 2))
                     .replace ("{N}", juce::String (used.size())).replace ("{M}", juce::String (master.effects.size()))
                   + "\n" + TRANS ("Untick the inserts you don't need: each one left out also leaves out its effects (less CPU). Sounds that went into it go to the master instead. Hover over an insert to see its effects."),
                   juce::dontSendNotification);
    intro.setColour (juce::Label::textColourId, col::text);
    intro.setFont (theme::ui (13.0f));
    intro.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (intro);

    for (auto n : used)
    {
        auto* row = rows.add (new Row (project, n));
        row->tick.onClick = [this] { updateImportButton(); };
        list.addAndMakeVisible (row);
    }

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    laneNames.setButtonText (TRANS ("Name the playlist tracks as in FL ({N} names)").replace ("{N}", juce::String (namedTracks)));
    laneNames.setToggleState (namedTracks > 0, juce::dontSendNotification);
    laneNames.setEnabled (namedTracks > 0);
    addAndMakeVisible (laneNames);

    int offEffects = 0;

    for (auto n : used)
        for (auto& fx : project.inserts.getReference (n).effects)
            offEffects += fx.enabled ? 0 : 1;

    for (auto& fx : master.effects)
        offEffects += fx.enabled ? 0 : 1;

    skipOff.setButtonText (TRANS ("Leave out effects that are off in FL ({N}): less CPU and memory").replace ("{N}", juce::String (offEffects)));
    skipOff.setToggleState (offEffects > 0, juce::dontSendNotification);
    skipOff.setEnabled (offEffects > 0);
    addAndMakeVisible (skipOff);

    allButton.setButtonText (TRANS ("All"));
    noneButton.setButtonText (TRANS ("None"));
    allButton.onClick  = [this] { for (auto* r : rows) r->tick.setToggleState (true, juce::dontSendNotification);  updateImportButton(); };
    noneButton.onClick = [this] { for (auto* r : rows) r->tick.setToggleState (false, juce::dontSendNotification); updateImportButton(); };

    importButton.setButtonText (TRANS ("Import"));
    importButton.setColour (juce::TextButton::buttonColourId, col::accent);
    importButton.setColour (juce::TextButton::textColourOffId, col::accentText);
    cancelButton.setButtonText (TRANS ("Cancel"));

    importButton.onClick = [this]
    {
        flimport::Options o;

        for (auto* r : rows)
            if (r->tick.getToggleState())
                o.inserts.add (r->number);

        o.namesOnPlaylist = laneNames.getToggleState();
        o.skipOffEffects = skipOff.getToggleState();
        auto callback = onImport;
        close();

        if (callback)
            callback (o);
    };

    cancelButton.onClick = [this] { close(); };

    for (auto* b : { &allButton, &noneButton, &importButton, &cancelButton })
        addAndMakeVisible (b);

    updateImportButton();
    setSize (640, 560);
}

FlpImportDialog::~FlpImportDialog() = default;

void FlpImportDialog::show (const flimport::Project& p, const juce::String& fileName,
                            std::function<void (const flimport::Options&)> onImport)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new FlpImportDialog (p, fileName, std::move (onImport)));
    o.dialogTitle = TRANS ("Import an FL Studio template");
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void FlpImportDialog::showText (const juce::String& title, const juce::String& text)
{
    auto* editor = new juce::TextEditor();
    editor->setMultiLine (true, true);
    editor->setReadOnly (true);
    editor->setScrollbarsShown (true);
    editor->setCaretVisible (false);
    editor->setFont (theme::ui (13.0f));
    editor->setColour (juce::TextEditor::backgroundColourId, col::well);
    editor->setColour (juce::TextEditor::textColourId, col::text);
    editor->setColour (juce::TextEditor::outlineColourId, col::line);
    editor->setText (text, false);
    editor->setSize (640, 460);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (editor);
    o.dialogTitle = title;
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void FlpImportDialog::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
        w->exitModalState (0);
}

void FlpImportDialog::updateImportButton()
{
    int ticked = 0;

    for (auto* r : rows)
        ticked += r->tick.getToggleState() ? 1 : 0;

    importButton.setButtonText (TRANS ("Import ({N} inserts)").replace ("{N}", juce::String (ticked)));
}

void FlpImportDialog::paint (juce::Graphics& g)
{
    g.fillAll (col::panel);
    g.setColour (col::line);
    g.drawRect (viewport.getBounds().expanded (1), 1);
}

void FlpImportDialog::resized()
{
    auto r = getLocalBounds().reduced (14);
    intro.setBounds (r.removeFromTop (78));
    r.removeFromTop (6);

    auto buttons = r.removeFromBottom (32);
    cancelButton.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (8);
    importButton.setBounds (buttons.removeFromRight (170));
    allButton.setBounds (buttons.removeFromLeft (70));
    buttons.removeFromLeft (6);
    noneButton.setBounds (buttons.removeFromLeft (70));

    r.removeFromBottom (8);
    skipOff.setBounds (r.removeFromBottom (26));
    laneNames.setBounds (r.removeFromBottom (26));
    r.removeFromBottom (6);

    viewport.setBounds (r);
    const int rowHeight = 28;
    list.setSize (viewport.getMaximumVisibleWidth(), rows.size() * rowHeight);

    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * rowHeight, list.getWidth(), rowHeight);
}
