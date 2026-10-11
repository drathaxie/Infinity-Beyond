using System.Security.Cryptography;
using Launcher;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (args.Length != 1 || !File.Exists(Path.Combine(args[0], "Assembly-CSharp.dll")))
    throw new ArgumentException("Supply the game's Managed directory; only temporary copies are patched.");

string fixture = Path.Combine(Path.GetTempPath(), "BeyondAssemblyChecks-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(fixture);
try
{
    foreach (string file in Directory.GetFiles(args[0], "*.dll"))
        File.Copy(file, Path.Combine(fixture, Path.GetFileName(file)));
    string assemblyPath = Path.Combine(fixture, "Assembly-CSharp.dll");
    // The installed client may already be patched. Remove only our startup call
    // from the disposable fixture so every run exercises a complete rewrite.
    using (DefaultAssemblyResolver resolver = new())
    {
        resolver.AddSearchDirectory(fixture);
        using MemoryStream input = new(File.ReadAllBytes(assemblyPath));
        using AssemblyDefinition clean = AssemblyDefinition.ReadAssembly(input, new ReaderParameters { AssemblyResolver = resolver });
        var start = clean.MainModule.Types.Single(t => t.Name == "AEC").Methods.Single(m => m.Name == "Start");
        var hooks = start.Body.Instructions.Where(i => i.OpCode == OpCodes.Call &&
            i.Operand is MethodReference m && m.DeclaringType.FullName == "BeyondAgent.Util.BeyondLifecycle" &&
            m.Name == "Create").ToArray();
        if (hooks.Length > 0)
        {
            foreach (var hook in hooks) start.Body.GetILProcessor().Remove(hook);
            using MemoryStream output = new();
            clean.Write(output);
            File.WriteAllBytes(assemblyPath, output.ToArray());
        }
    }
    byte[] original = File.ReadAllBytes(assemblyPath);
    string hash = Convert.ToHexString(SHA256.HashData(original));

    void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
        Console.WriteLine("PASS " + message);
    }
    void Unchanged(string message) => Check(File.ReadAllBytes(assemblyPath).SequenceEqual(original), message);

    // Use a real dependency needed by the current client's enum metadata. This
    // reproduces the write-time failure that used to truncate the installed DLL.
    string audio = Path.Combine(fixture, "UnityEngine.AudioModule.dll");
    File.Move(audio, audio + ".held");
    try
    {
        AssemblyPatcher.Patch(fixture);
        throw new Exception("Expected missing dependency to fail.");
    }
    catch (AssemblyResolutionException) { }
    finally { File.Move(audio + ".held", audio); }
    Unchanged("missing dependency preserves the original game DLL");

    // Simulate a game process holding the installed assembly without delete sharing.
    using (FileStream locked = new(assemblyPath, FileMode.Open, FileAccess.Read, FileShare.Read))
    {
        bool failed = false;
        try { AssemblyPatcher.Patch(fixture); }
        catch (IOException) { failed = true; }
        catch (UnauthorizedAccessException) { failed = true; }
        Check(failed, "locked destination rejects replacement");
    }
    Unchanged("failed replacement preserves the original game DLL");
    Check(Directory.GetFiles(fixture, "*.tmp").Length == 0, "failed replacement cleans staged output");

    AssemblyPatcher.Patch(fixture);
    byte[] patched = File.ReadAllBytes(assemblyPath);
    using (AssemblyDefinition result = AssemblyDefinition.ReadAssembly(new MemoryStream(patched)))
    {
        var start = result.MainModule.Types.Single(t => t.Name == "AEC").Methods.Single(m => m.Name == "Start");
        Check(start.Body.Instructions.Count(i => i.OpCode == OpCodes.Call &&
            i.Operand is MethodReference m && m.DeclaringType.FullName == "BeyondAgent.Util.BeyondLifecycle" &&
            m.Name == "Create") == 1, "current client contains exactly one Beyond startup call");
    }
    Check(File.ReadAllBytes(assemblyPath + ".unpatched-" + hash + ".bak").SequenceEqual(original),
        "backup matches this exact client version");
    AssemblyPatcher.Patch(fixture);
    Check(File.ReadAllBytes(assemblyPath).SequenceEqual(patched), "second launch leaves the patched DLL unchanged");
}
finally { Directory.Delete(fixture, true); }
