// Scriba -> Scriba Pitch through Meteora Link, with the real Scriba plug-in.
//
//   ScribaPitchLinkTest <Scriba.vst3>
//
// Loads the real Scriba.vst3, chooses a key in it (D minor) and checks what Scriba
// Pitch's link reader sees: in the same process (most DAWs), in another process
// (DAWs that run plug-ins in their own process), after 12 s (heartbeat keeps it
// alive) and after Scriba is removed (the key disappears).

#include <juce_audio_processors/juce_audio_processors.h>
#include "Link/MeteoraLink.h"
#include <cstdio>

static void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
}

static meteora::link::Combined readAsScribaPitch()
{
    return meteora::link::combine (meteora::link::readOthers ("scriba-pitch-test"), "Scriba");
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 1 && juce::String (argv[1]) == "--reader")
    {
        const auto c = readAsScribaPitch();
        std::printf ("%d %s\n", c.key, c.sources.joinIntoString ("+").toRawUTF8());
        return 0;
    }
    if (argc < 2) { std::printf ("usage: ScribaPitchLinkTest <Scriba.vst3>\n"); return 1; }

    int failures = 0;
    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> types;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (types, juce::String (argv[1]));
    if (types.isEmpty()) { std::printf ("cannot load %s\n", argv[1]); return 1; }

    juce::String err;
    auto scriba = fm.createPluginInstance (*types[0], 48000.0, 512, err);
    if (scriba == nullptr) { std::printf ("cannot create Scriba: %s\n", err.toRawUTF8()); return 1; }
    scriba->prepareToPlay (48000.0, 512);
    std::printf ("Scriba loaded: %s\n", scriba->getName().toRawUTF8());

    auto before = readAsScribaPitch();
    std::printf ("before choosing a key: Scriba Pitch sees key %d (expected -1)\n", before.key);
    if (before.key != -1) ++failures;

    // Scriba: instrument GUITAR, key D minor (choice 1 + 2*pc + minor)
    for (auto* p : scriba->getParameters())
    {
        auto* wp = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
        const auto id = wp != nullptr ? wp->getParameterID() : juce::String();
        if (p->getName (64) == "Instrument" || id == "instrument")
            p->setValueNotifyingHost (1.0f / (float) juce::jmax (1, p->getNumSteps() - 1));   // GUITAR (index 1 of 6)
        if (p->getName (64) == "Song key" || id == "songKey")
        {
            const int steps = p->getNumSteps();
            p->setValueNotifyingHost ((float) (1 + 2 * 2 + 1) / (float) juce::jmax (1, steps - 1));
        }
    }
    pump (800);

    auto same = readAsScribaPitch();
    std::printf ("same process: Scriba Pitch sees %s from %s: %s\n", meteora::link::Combined().key == same.key ? "nothing" : "a key",
                 same.sources.joinIntoString ("+").toRawUTF8(), same.key == 14 ? "D minor, yes" : "NO");
    if (same.key != 14) ++failures;

    auto runReader = [&]
    {
        juce::ChildProcess child;
        child.start (juce::StringArray { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(), "--reader" });
        const auto out = child.readAllProcessOutput().trim();
        return out;
    };

    auto other = runReader();
    std::printf ("another process: sees \"%s\": %s\n", other.toRawUTF8(), other.startsWith ("14 ") ? "yes" : "NO");
    if (! other.startsWith ("14 ")) ++failures;

    pump (12000);   // the heartbeat must keep it alive for the other process
    other = runReader();
    std::printf ("another process, 12 s later: sees \"%s\": %s\n", other.toRawUTF8(), other.startsWith ("14 ") ? "yes" : "NO");
    if (! other.startsWith ("14 ")) ++failures;

    scriba->releaseResources();
    scriba.reset();
    pump (300);
    auto after = readAsScribaPitch();
    std::printf ("Scriba removed: Scriba Pitch sees key %d (expected -1)\n", after.key);
    if (after.key != -1) ++failures;

    std::printf (failures == 0 ? "LINK TEST PASSED\n" : "LINK TEST FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
