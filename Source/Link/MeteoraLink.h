#pragma once

/*  Meteora Link — lets Meteora Plugins instances loaded in the same DAW share
    the song key and chords, even when they are different plugins (Scriba and
    Scriba Pitch are separate binaries, so static variables cannot be shared).

    Each instance writes one small JSON file in
        <user app data>/Meteora Plugins/Link/<pid>_<instance id>.json
    and reads the files written by the other instances.
    Same DAW process: always read. Another process (DAWs that run plug-ins in a
    separate process: Bitwig, Logic's AU host, REAPER "run as separate process"):
    read while it is alive — every instance re-touches its file every 2 s, and a
    file not touched for 8 s is ignored (closed or crashed). Files left by a crash
    are removed after 2 days.

    Header-only, JUCE (juce_core + juce_events): drop this same file into Scriba.
*/

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <vector>
#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <unistd.h>
#endif

namespace meteora::link
{

constexpr int protocolVersion = 1;
constexpr juce::int64 heartbeatMs = 2000, aliveMs = 8000;

struct Chord
{
    double ppq = 0.0;        // start, in quarter notes from the project start
    double lengthPpq = 0.0;
    juce::String name;       // "Am", "F", "G7"...
};

struct Info
{
    juce::String plugin;      // "Scriba", "Scriba Pitch"
    juce::String instrument;  // "BASS", "GUITAR", "PIANO", "VOCALS", "KICK", "SNARE"...
    int key = -1;             // 0..11 = C..B major, 12..23 = C..B minor, -1 = none
    int alt = -1;
    float confidence = 0.0f;  // 0..1
    std::vector<Chord> chords;
    juce::int64 updated = 0;  // ms since epoch
    juce::String instanceId;
};

//==============================================================================
inline juce::File folder()
{
   #if JUCE_MAC
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    return base.getChildFile ("Meteora Plugins").getChildFile ("Link");
}

inline juce::String processId()
{
   #if JUCE_WINDOWS
    return juce::String ((juce::int64) ::_getpid());
   #else
    return juce::String ((juce::int64) ::getpid());
   #endif
}

inline juce::var toVar (const Info& info)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("version", protocolVersion);
    o->setProperty ("plugin", info.plugin);
    o->setProperty ("instrument", info.instrument);
    o->setProperty ("key", info.key);
    o->setProperty ("alt", info.alt);
    o->setProperty ("confidence", info.confidence);
    o->setProperty ("updated", juce::Time::currentTimeMillis());
    juce::Array<juce::var> chords;
    for (const auto& c : info.chords)
    {
        auto* co = new juce::DynamicObject();
        co->setProperty ("ppq", c.ppq);
        co->setProperty ("len", c.lengthPpq);
        co->setProperty ("name", c.name);
        chords.add (juce::var (co));
    }
    o->setProperty ("chords", chords);
    return juce::var (o);
}

inline bool fromVar (const juce::var& v, Info& info)
{
    if (! v.isObject() || (int) v.getProperty ("version", 0) != protocolVersion)
        return false;
    info.plugin = v.getProperty ("plugin", {}).toString();
    info.instrument = v.getProperty ("instrument", {}).toString();
    info.key = juce::jlimit (-1, 23, (int) v.getProperty ("key", -1));
    info.alt = juce::jlimit (-1, 23, (int) v.getProperty ("alt", -1));
    info.confidence = juce::jlimit (0.0f, 1.0f, (float) (double) v.getProperty ("confidence", 0.0));
    info.updated = (juce::int64) v.getProperty ("updated", 0);
    info.chords.clear();
    if (auto* arr = v.getProperty ("chords", {}).getArray())
        for (const auto& c : *arr)
            info.chords.push_back ({ (double) c.getProperty ("ppq", 0.0), (double) c.getProperty ("len", 0.0),
                                     c.getProperty ("name", {}).toString() });
    return true;
}

//==============================================================================
/** One per plugin instance. publish() whenever the results change (any thread
    except the audio thread). The file is kept alive by a 2 s heartbeat. */
class Publisher  : private juce::Timer
{
public:
    Publisher() : id (juce::Uuid().toString().substring (0, 12)) { startTimer ((int) heartbeatMs); }
    ~Publisher() override { stopTimer(); withdraw(); }

    const juce::String& getInstanceId() const noexcept { return id; }

    void publish (const Info& info)
    {
        const juce::ScopedLock sl (lock);
        text = juce::JSON::toString (toVar (info), true);
        writeLocked();
    }

    void withdraw()
    {
        const juce::ScopedLock sl (lock);
        text.clear();
        file().deleteFile();
    }

private:
    void timerCallback() override
    {
        const juce::ScopedLock sl (lock);
        if (text.isEmpty()) return;
        const auto f = file();
        if (! f.existsAsFile()) writeLocked();   // removed by someone: write it again
        else f.setLastModificationTime (juce::Time::getCurrentTime());
    }

    void writeLocked()
    {
        auto dir = folder();
        dir.createDirectory();
        file().replaceWithText (text);
    }

    juce::File file() const { return folder().getChildFile (processId() + "_" + id + ".json"); }
    juce::String id, text;
    juce::CriticalSection lock;
};

/** Everything the other instances in this DAW have published (not including `selfId`). */
inline std::vector<Info> readOthers (const juce::String& selfId)
{
    std::vector<Info> out;
    const auto dir = folder();
    if (! dir.isDirectory())
        return out;

    const auto prefix = processId() + "_";
    const auto now = juce::Time::currentTimeMillis();
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto name = f.getFileNameWithoutExtension();
        const auto age = now - f.getLastModificationTime().toMilliseconds();
        if (! name.startsWith (prefix))
        {
            if (age > 2LL * 24 * 3600 * 1000)
                f.deleteFile();   // left over from a crash
            if (age > aliveMs)
                continue;         // another process, not alive any more
        }
        const auto inst = name.fromFirstOccurrenceOf ("_", false, false);
        if (inst == selfId)
            continue;

        Info info;
        if (fromVar (juce::JSON::parse (f), info))
        {
            info.instanceId = inst;
            out.push_back (std::move (info));
        }
    }
    return out;
}

/** Song key and chords from the given instances (instrument parts weigh more than vocals). */
struct Combined
{
    int key = -1, alt = -1;
    float confidence = 0.0f;
    std::vector<Chord> chords;
    juce::StringArray sources;   // e.g. "BASS", "GUITAR"
};

inline Combined combine (const std::vector<Info>& infos, const juce::String& onlyPlugin = {})
{
    Combined c;
    float score[24] {};
    size_t mostChords = 0;

    for (const auto& i : infos)
    {
        if (onlyPlugin.isNotEmpty() && i.plugin != onlyPlugin) continue;
        if (i.key < 0) continue;
        const auto inst = i.instrument.toUpperCase();
        const float w = (inst == "BASS" || inst == "GUITAR" || inst == "PIANO") ? 1.0f : 0.6f;
        score[i.key] += w * juce::jmax (0.2f, i.confidence);
        if (i.alt >= 0) score[i.alt] += 0.25f * w * juce::jmax (0.2f, i.confidence);
        c.sources.addIfNotAlreadyThere (inst.isNotEmpty() ? inst : i.plugin.toUpperCase());

        if (i.chords.size() > mostChords)
        {
            mostChords = i.chords.size();
            c.chords = i.chords;
        }
    }

    float total = 0.0f;
    for (int k = 0; k < 24; ++k)
    {
        total += score[k];
        if (score[k] > 0.0f && (c.key < 0 || score[k] > score[c.key])) c.key = k;
    }
    for (int k = 0; k < 24; ++k)
        if (k != c.key && score[k] > 0.0f && (c.alt < 0 || score[k] > score[c.alt])) c.alt = k;
    juce::ignoreUnused (total);
    if (c.key >= 0)
    {
        // confidence = average of the instances that agree, lowered by those that disagree
        float agreeW = 0.0f, agreeConf = 0.0f, allW = 0.0f;
        for (const auto& i : infos)
        {
            if ((onlyPlugin.isNotEmpty() && i.plugin != onlyPlugin) || i.key < 0) continue;
            const auto inst = i.instrument.toUpperCase();
            const float w = (inst == "BASS" || inst == "GUITAR" || inst == "PIANO") ? 1.0f : 0.6f;
            allW += w;
            if (i.key == c.key) { agreeW += w; agreeConf += w * i.confidence; }
        }
        if (agreeW > 0.0f)
            c.confidence = juce::jlimit (0.0f, 0.99f, (agreeConf / agreeW) * (agreeW / allW));
    }
    return c;
}

} // namespace meteora::link
