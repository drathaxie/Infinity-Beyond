using Mono.Cecil;
using Mono.Cecil.Cil;
using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;

namespace Launcher
{
    public static class AssemblyPatcher
    {
        public static void Patch(string managedDir)
        {
            string assemblyPath = Path.Combine(managedDir, "Assembly-CSharp.dll");
            string agentPath = Path.Combine(managedDir, "BeyondAgent.dll");

            if (!File.Exists(assemblyPath))
            {
                throw new FileNotFoundException("Assembly-CSharp.dll not found", assemblyPath);
            }
            if (!File.Exists(agentPath))
            {
                throw new FileNotFoundException("BeyondAgent.dll not found", agentPath);
            }

            // Read the Assembly-CSharp assembly into memory first to avoid file locks when writing back
            byte[] assemblyBytes = File.ReadAllBytes(assemblyPath);
            // Cecil may resolve enum parameter types while writing metadata. Resolve
            // against this game's assemblies, not the launcher's working directory.
            using DefaultAssemblyResolver resolver = new();
            resolver.AddSearchDirectory(managedDir);
            ReaderParameters readerParameters = new() { AssemblyResolver = resolver };
            using MemoryStream ms = new(assemblyBytes);
            using AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(ms, readerParameters);
            TypeDefinition? aecType = assembly.MainModule.Types.FirstOrDefault(t => t.Name == "AEC");
            if (aecType == null)
            {
                throw new TypeLoadException("Could not find class AEC in Assembly-CSharp.dll");
            }

            MethodDefinition? startMethod = aecType.Methods.FirstOrDefault(m => m.Name == "Start");
            if (startMethod == null)
            {
                throw new EntryPointNotFoundException("Could not find method AEC.Start in Assembly-CSharp.dll");
            }

            if (!startMethod.HasBody)
            {
                throw new InvalidOperationException("AEC.Start method has no body.");
            }

            // Check if already patched: does it call BeyondLifecycle.Create?
            bool alreadyPatched = false;
            foreach (Instruction? instruction in startMethod.Body.Instructions)
            {
                if (instruction.OpCode == OpCodes.Call &&
                    instruction.Operand is MethodReference methodRef &&
                    methodRef.DeclaringType.FullName == "BeyondAgent.Util.BeyondLifecycle" &&
                    methodRef.Name == "Create")
                {
                    alreadyPatched = true;
                    break;
                }
            }

            if (alreadyPatched)
            {
                System.Diagnostics.Trace.WriteLine("Assembly-CSharp.dll is already patched. Skipping.");
                return;
            }

            // Load agent assembly to resolve BeyondLifecycle.Create
            using (AssemblyDefinition agentAssembly = AssemblyDefinition.ReadAssembly(agentPath, readerParameters))
            {
                TypeDefinition? agentType = agentAssembly.MainModule.Types.FirstOrDefault(t => t.FullName == "BeyondAgent.Util.BeyondLifecycle");
                if (agentType == null)
                {
                    throw new TypeLoadException("Could not find class BeyondAgent.Util.BeyondLifecycle in BeyondAgent.dll");
                }

                MethodDefinition? createMethod = agentType.Methods.FirstOrDefault(m => m.Name == "Create" && m.Parameters.Count == 0);
                if (createMethod == null)
                {
                    throw new EntryPointNotFoundException("Could not find method Create in BeyondAgent.BeyondLifecycle");
                }

                // Import the method reference
                MethodReference createMethodRef = assembly.MainModule.ImportReference(createMethod);

                // Insert the call instruction at the very beginning of AEC.Start()
                ILProcessor il = startMethod.Body.GetILProcessor();
                Instruction firstInstruction = startMethod.Body.Instructions[0];
                Instruction callInstruction = il.Create(OpCodes.Call, createMethodRef);
                il.InsertBefore(firstInstruction, callInstruction);
            }

            // Never let Cecil open/truncate the installed DLL. Finish and validate
            // the entire rewrite in memory before touching any installed files.
            using MemoryStream output = new();
            assembly.Write(output);
            byte[] patchedBytes = output.ToArray();
            using (MemoryStream validationStream = new(patchedBytes))
            using (AssemblyDefinition validated = AssemblyDefinition.ReadAssembly(validationStream, readerParameters))
            {
                MethodDefinition? validatedStart = validated.MainModule.Types
                    .FirstOrDefault(t => t.Name == "AEC")?.Methods.FirstOrDefault(m => m.Name == "Start");
                if (validatedStart == null || !validatedStart.HasBody ||
                    !validatedStart.Body.Instructions.Any(i => i.OpCode == OpCodes.Call &&
                        i.Operand is MethodReference m &&
                        m.DeclaringType.FullName == "BeyondAgent.Util.BeyondLifecycle" && m.Name == "Create"))
                {
                    throw new InvalidOperationException("Patched assembly failed BeyondAgent startup validation.");
                }
            }

            // Keep the legacy backup and also preserve each new Steam version.
            // A .bak from a previous release cannot safely restore today's client.
            string originalHash = Convert.ToHexString(SHA256.HashData(assemblyBytes));
            string backupPath = assemblyPath + ".unpatched-" + originalHash + ".bak";
            if (!File.Exists(backupPath))
            {
                File.WriteAllBytes(backupPath, assemblyBytes);
            }
            string legacyBackupPath = assemblyPath + ".bak";
            if (!File.Exists(legacyBackupPath))
            {
                File.WriteAllBytes(legacyBackupPath, assemblyBytes);
            }

            string stagedPath = assemblyPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllBytes(stagedPath, patchedBytes);
                // Refuse to overwrite a concurrent Steam update or another patch.
                if (!File.ReadAllBytes(assemblyPath).SequenceEqual(assemblyBytes))
                {
                    throw new IOException("Assembly-CSharp.dll changed during patching. Retry after the game update finishes.");
                }
                File.Replace(stagedPath, assemblyPath, null);
            }
            finally
            {
                if (File.Exists(stagedPath)) File.Delete(stagedPath);
            }
            System.Diagnostics.Trace.WriteLine("Successfully patched Assembly-CSharp.dll to load Standalone Launcher Agent.");
        }
    }
}
