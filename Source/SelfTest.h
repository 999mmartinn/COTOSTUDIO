#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

class SelfTest
{
public:
    SelfTest (te::Engine&, const juce::StringArray& args, std::function<void (int exitCode)> onFinished);
    ~SelfTest();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
