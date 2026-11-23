// openTrainSim .NET train plugin host.
//
// Runs an openBVE train plugin (a managed assembly implementing OpenBveApi.Runtime.IRuntime,
// e.g. ATS / ATC / ATP / TVM safety systems) in its own Mono process, like openBVE's Win32
// plugin proxy. The simulator talks to it over stdin / stdout with one command per line:
//
//   STATION <name>\t<arr>\t<dep>\t<stop>\t<fwd>\t<bwd>\t<left>\t<right>\t<mode>   (before LOAD)
//   LOAD <powerNotches> <brakeType> <brakeNotches> <holdBrake> <cars> <initMode>
//        → "OK <panelSize>" | "FAIL <reason>"
//   ELAPSE <location> <kmh> <bc> <mr> <er> <bp> <sap> <radius> <cant> <pitch> <totalMs> <elapsedMs>
//          <rev> <pow> <brk> <constSpeed> <holdBrake> <doorInterlock> <headlights> <camera>
//        → "H <rev> <pow> <brk> <constSpeed> <holdBrake> <doorInterlock> <headlights>",
//          "P <n> <values...>", any "MSG <rgb> <seconds> <text>", "DOORS <open|close> <l> <r>",
//          "DEBUG <text>", "ERR <text>",
//          "SND <id> <car> <index> <volume> <pitch> <looped 0|1>" (car -1 is the driver panel),
//          "SNDSET <id> <volume> <pitch>", "SNDSTOP <id>", then "END"
//        Sound starts queued during or between frames precede sound updates and stops.
//   SETREV n | SETPOW n | SETBRK n | KEYDOWN k | KEYUP k | HORN t | DOOR old new | INIT mode
//   SIGNAL <n> <aspect> <distance> ...   BEACON <type> <optional> <aspect> <distance>
//   SNDEND <id>   (playback finished or was dropped; invalidates the plugin's handle)
//   QUIT
// Commands other than LOAD / ELAPSE have no reply; their exceptions are reported with the
// next ELAPSE.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using OpenBveApi.Runtime;

internal sealed class HostSound : SoundHandle
{
    public readonly int Id;
    public double ReportedVolume;
    public double ReportedPitch;

    public HostSound(int id, double volume, double pitch)
    {
        Id = id;
        MyValid = true;
        MyVolume = ReportedVolume = volume;
        MyPitch = ReportedPitch = pitch;
    }

    public void Finish() { MyValid = false; }
}

internal static class Program
{
    static readonly CultureInfo C = CultureInfo.InvariantCulture;
    static IRuntime api;
    static int[] panel = new int[0];
    static readonly List<string> pending = new List<string>(); // messages to send with the next ELAPSE
    static readonly List<Station> stations = new List<Station>();
    static readonly List<HostSound> sounds = new List<HostSound>();
    static int nextSoundId = 1;

    static double D(string s) { return double.Parse(s, C); }
    static int I(string s) { return int.Parse(s, C); }

    static SoundHandle PlaySound(int index, double volume, double pitch, bool looped, int car)
    {
        var sound = new HostSound(nextSoundId++, volume, pitch);
        sounds.Add(sound);
        pending.Add(string.Format(C, "SND {0} {1} {2} {3:R} {4:R} {5}",
                                  sound.Id, car, index, volume, pitch, looped ? 1 : 0));
        return sound;
    }

    static void SoundEnded(int id)
    {
        int index = sounds.FindIndex(sound => sound.Id == id);
        if (index < 0) return;
        sounds[index].Finish();
        sounds.RemoveAt(index);
    }

    static void Fail(Exception e)
    {
        Exception inner = e is TargetInvocationException && e.InnerException != null ? e.InnerException : e;
        pending.Add("ERR " + inner.GetType().Name + ": " + inner.Message.Replace('\n', ' '));
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: PluginHost <plugin.dll> <train folder>");
            return 2;
        }
        var stdin = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
        var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = false, NewLine = "\n" };
        string pluginFile = Path.GetFullPath(args[0]);
        string trainFolder = Path.GetFullPath(args[1]);
        // plugins may resolve files relative to the working directory
        try { Directory.SetCurrentDirectory(Path.GetDirectoryName(pluginFile)); } catch { }

        string line;
        while ((line = stdin.ReadLine()) != null)
        {
            if (line.Length == 0) continue;
            int sp = line.IndexOf(' ');
            string cmd = sp < 0 ? line : line.Substring(0, sp);
            string rest = sp < 0 ? "" : line.Substring(sp + 1);
            string[] a = rest.Length > 0 ? rest.Split(' ') : new string[0];
            try
            {
                switch (cmd)
                {
                    case "STATION":
                    {
                        string[] f = rest.Split('\t');
                        var s = new Station();
                        s.Name = f[0];
                        s.ArrivalTime = D(f[1]);
                        s.DepartureTime = D(f[2]);
                        s.StopPosition = D(f[3]);
                        s.ForwardTolerance = D(f[4]);
                        s.BackwardTolerance = D(f[5]);
                        s.OpenLeftDoors = f[6] == "1";
                        s.OpenRightDoors = f[7] == "1";
                        s.StopMode = (StationStopMode)I(f[8]);
                        stations.Add(s);
                        break;
                    }
                    case "LOAD":
                        stdout.WriteLine(Load(pluginFile, trainFolder, a));
                        stdout.Flush();
                        break;
                    case "ELAPSE":
                        Elapse(a, stdout);
                        stdout.Flush();
                        break;
                    case "SETREV": api.SetReverser(I(a[0])); break;
                    case "SETPOW": api.SetPower(I(a[0])); break;
                    case "SETBRK": api.SetBrake(I(a[0])); break;
                    case "KEYDOWN": api.KeyDown((VirtualKeys)I(a[0])); break;
                    case "KEYUP": api.KeyUp((VirtualKeys)I(a[0])); break;
                    case "HORN": api.HornBlow((HornTypes)I(a[0])); break;
                    case "DOOR": api.DoorChange((DoorStates)I(a[0]), (DoorStates)I(a[1])); break;
                    case "INIT": api.Initialize((InitializationModes)I(a[0])); break;
                    case "SNDEND": SoundEnded(I(a[0])); break;
                    case "SIGNAL":
                    {
                        int n = I(a[0]);
                        var data = new SignalData[n];
                        for (int i = 0; i < n; i++) data[i] = new SignalData(I(a[1 + 2 * i]), D(a[2 + 2 * i]));
                        api.SetSignal(data);
                        break;
                    }
                    case "BEACON":
                        api.SetBeacon(new BeaconData(I(a[0]), I(a[1]), new SignalData(I(a[2]), D(a[3]))));
                        break;
                    case "QUIT":
                        try { if (api != null) api.Unload(); } catch { }
                        return 0;
                }
            }
            catch (Exception e)
            {
                Fail(e);
                if (cmd == "LOAD" || cmd == "ELAPSE")
                {
                    foreach (string m in pending) stdout.WriteLine(m);
                    pending.Clear();
                    stdout.WriteLine(cmd == "LOAD" ? "FAIL exception" : "END");
                    stdout.Flush();
                }
            }
        }
        return 0;
    }

    static string Load(string pluginFile, string trainFolder, string[] a)
    {
        Assembly asm = Assembly.LoadFile(pluginFile);
        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException e) { types = e.Types; }
        foreach (Type t in types)
        {
            if (t == null || !t.IsClass || t.IsAbstract || !typeof(IRuntime).IsAssignableFrom(t)) continue;
            api = (IRuntime)Activator.CreateInstance(t);
            break;
        }
        if (api == null) return "FAIL no class implementing OpenBveApi.Runtime.IRuntime in " + Path.GetFileName(pluginFile);

        PlaySoundDelegate playSound = (index, volume, pitch, looped) => PlaySound(index, volume, pitch, looped, -1);
        PlayCarSoundDelegate playCarSound = PlaySound;
        PlayMultipleCarSoundDelegate playMulti = (index, volume, pitch, looped, cars) =>
        {
            var carSounds = new SoundHandle[cars.Length];
            for (int i = 0; i < cars.Length; i++)
                carSounds[i] = PlaySound(index, volume, pitch, looped, cars[i]);
            return carSounds;
        };
        AddInterfaceMessageDelegate addMessage = (message, color, time) =>
            pending.Add("MSG " + ((int)color).ToString(C) + " " + time.ToString(C) + " " + (message ?? "").Replace('\n', ' '));
        AddScoreDelegate addScore = (score, message, color, time) =>
            pending.Add("MSG " + ((int)color).ToString(C) + " " + time.ToString(C) + " " + (message ?? "").Replace('\n', ' '));
        OpenDoorsDelegate openDoors = (l, r) => pending.Add("DOORS open " + (l ? 1 : 0) + " " + (r ? 1 : 0));
        CloseDoorsDelegate closeDoors = (l, r) => pending.Add("DOORS close " + (l ? 1 : 0) + " " + (r ? 1 : 0));
        var props = new LoadProperties(Path.GetDirectoryName(pluginFile), trainFolder, playSound, playCarSound, playMulti, addMessage, addScore,
                                       openDoors, closeDoors);
        if (!api.Load(props)) return "FAIL " + (props.FailureReason ?? "the plugin refused to load");
        panel = props.Panel ?? new int[0];
        var specs = new VehicleSpecs(I(a[0]), (BrakeTypes)I(a[1]), I(a[2]), a[3] == "1", false, I(a[4]), 2);
        api.SetVehicleSpecs(specs);
        api.Initialize((InitializationModes)I(a[5]));
        return "OK " + panel.Length.ToString(C);
    }

    static void Elapse(string[] a, StreamWriter o)
    {
        var vehicle = new VehicleState(D(a[0]), new Speed(D(a[1]) / 3.6), D(a[2]), D(a[3]), D(a[4]), D(a[5]), D(a[6]), D(a[7]), D(a[8]), D(a[9]));
        var preceding = new PrecedingVehicleState(double.MaxValue, double.MaxValue, new Speed(0.0));
        var handles = new Handles(I(a[12]), I(a[13]), I(a[14]), 0, a[15] == "1", a[16] == "1");
        var data = new ElapseData(vehicle, preceding, handles, (DoorInterlockStates)I(a[17]), I(a[18]), new Time(D(a[10]) / 1000.0),
                                  new Time(D(a[11]) / 1000.0), stations, (CameraViewMode)I(a[19]), "ko-KR", 0);
        api.Elapse(data);
        Handles h = data.Handles;
        o.WriteLine("H " + h.Reverser + " " + h.PowerNotch + " " + h.BrakeNotch + " " + (h.ConstSpeed ? 1 : 0) + " " + (h.HoldBrake ? 1 : 0) + " " +
                    (int)data.DoorInterlockState + " " + data.HeadlightState);
        var sb = new StringBuilder("P ");
        sb.Append(panel.Length.ToString(C));
        for (int i = 0; i < panel.Length; i++) sb.Append(' ').Append(panel[i].ToString(C));
        o.WriteLine(sb.ToString());
        if (!string.IsNullOrEmpty(data.DebugMessage)) o.WriteLine("DEBUG " + data.DebugMessage.Replace('\n', ' '));
        foreach (string m in pending) o.WriteLine(m);
        pending.Clear();
        for (int i = 0; i < sounds.Count;)
        {
            HostSound sound = sounds[i];
            if (sound.Stopped)
            {
                o.WriteLine(string.Format(C, "SNDSTOP {0}", sound.Id));
                sounds.RemoveAt(i);
                continue;
            }
            if (!sound.Volume.Equals(sound.ReportedVolume) || !sound.Pitch.Equals(sound.ReportedPitch))
            {
                o.WriteLine(string.Format(C, "SNDSET {0} {1:R} {2:R}", sound.Id, sound.Volume, sound.Pitch));
                sound.ReportedVolume = sound.Volume;
                sound.ReportedPitch = sound.Pitch;
            }
            i++;
        }
        o.WriteLine("END");
    }
}
