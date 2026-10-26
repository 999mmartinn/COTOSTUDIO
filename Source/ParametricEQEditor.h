#pragma once

#include "ParametricEQPlugin.h"
#include "Look.h"

//==============================================================================
/**
    The Parametric EQ's window: the curve with one point per band, a live spectrum
    behind it, and the selected band's controls below.

      drag a point          frequency and gain (Ctrl: fine)
      mouse wheel           band width (Q)
      Shift held            hear only that band's range (this PC only)
      Alt+click             reset the band
      double-click          turn the band off / on
      right-click           type and slope menu
*/
class ParametricEQEditor  : public juce::Component,
                            private juce::Timer
{
public:
    explicit ParametricEQEditor (ParametricEQPlugin&);
    ~ParametricEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    ParametricEQPlugin& plugin;
    std::array<peq::Band, peq::numBands> shown {};
    double shownMain = 0.0;
    int selected = 3, hovered = -1, dragging = -1;
    juce::Point<float> dragStart;
    double dragStartFreq = 0.0, dragStartGain = 0.0;
    juce::uint32 lastWheelTime = 0;
    std::array<int, peq::numBands> lastOnType {};
    bool updatingControls = false;

    // Curve and spectrum
    juce::Path curve;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> fftData, window, ring;
    int ringPos = 0;
    std::vector<float> spectrumDb;   // smoothed, per display column
    juce::Path spectrumPath;

    // Selected band's controls
    juce::ComboBox typeBox, slopeBox;
    ResetSlider freqSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                qSlider    { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                mainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label freqLabel, gainLabel, qLabel, mainLabel;
    juce::OwnedArray<juce::TextButton> bandButtons;

    juce::Rectangle<int> graphArea() const;
    float xForFreq (double hz) const;
    double freqForX (float x) const;
    float yForDb (double db) const;
    double dbForY (float y) const;
    juce::Point<float> pointFor (int band) const;
    int bandAt (juce::Point<float>) const;

    void timerCallback() override;
    void updateCurve();
    void updateSpectrum();
    void syncControls();
    void selectBand (int);
    void beginEdit();
    void setBand (int index, const peq::Band&);
    void showBandMenu (int band);
    void setSolo (bool on);

    static juce::Colour bandColour (int band);
    static constexpr double minHz = 20.0, maxHz = 20000.0, dbRange = 18.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEQEditor)
};
