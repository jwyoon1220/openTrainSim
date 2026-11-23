#pragma once
// openBVE train sound configuration: BVE4 sound.cfg ("Version 1.0" header, [Section] blocks with
// "key = file" or "index = file" lines). When the train has no sound.cfg, the BVE2 default file
// names in the train folder are used (as openBVE's SoundCfg.Bve2.cs does).
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ots {

struct TrainSoundFiles {
    // indexed sections (index → absolute path); "[Switch]\nPoint.wav" (no index) means index 0
    std::map<int, std::filesystem::path> run, flange, motor, pointSwitch, ats;
    // [Brake]
    std::filesystem::path bcReleaseHigh, bcRelease, bcReleaseFull, emergency, emergencyRelease, bpDecomp;
    // [Compressor]
    std::filesystem::path compressorAttack, compressorLoop, compressorRelease;
    // [Suspension]
    std::filesystem::path springLeft, springRight;
    // [Horn]: BVE4 "Primary/Secondary/Music" are single sounds; openBVE also accepts
    // PrimaryStart/PrimaryLoop/PrimaryEnd (same for Secondary, Music).
    std::filesystem::path hornPrimary, hornPrimaryStart, hornPrimaryLoop, hornPrimaryEnd;
    std::filesystem::path hornSecondary, hornSecondaryStart, hornSecondaryLoop, hornSecondaryEnd;
    std::filesystem::path hornMusic, hornMusicStart, hornMusicLoop, hornMusicEnd;
    // [Door]
    std::filesystem::path doorOpenLeft, doorOpenRight, doorCloseLeft, doorCloseRight;
    // [Buzzer] Correct, [Pilot Lamp] On/Off
    std::filesystem::path buzzerCorrect, pilotLampOn, pilotLampOff;
    // [Brake Handle] Apply/ApplyFast/Release/ReleaseFast/Min/Max
    std::filesystem::path brakeApply, brakeApplyFast, brakeRelease, brakeReleaseFast, brakeMin, brakeMax;
    // [Master Controller] Up/UpFast/Down/DownFast/Min/Max
    std::filesystem::path powerUp, powerUpFast, powerDown, powerDownFast, powerMin, powerMax;
    // [Reverser] On/Off, [Breaker] On/Off
    std::filesystem::path reverserOn, reverserOff, breakerOn, breakerOff;
    // [Others] Noise/Shoe/Halt
    std::filesystem::path noise, shoe, halt;
    std::vector<std::string> warnings; // missing files, unknown keys (file:line: message)
};

// `trainFolder` is the folder containing train.dat. Paths in the file are relative to the train
// folder; resolve them case-insensitively (util/Paths.hpp resolvePath). Files that do not exist
// are left empty and reported in `warnings`. Returns false only if neither sound.cfg nor any
// BVE2 default file exists.
bool loadTrainSounds(const std::filesystem::path& trainFolder, TrainSoundFiles& out);

} // namespace ots
