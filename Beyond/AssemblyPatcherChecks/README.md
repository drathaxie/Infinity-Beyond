Run against the current game's Managed directory:

```powershell
dotnet run --project Beyond/AssemblyPatcherChecks -c Release -- "C:\path\to\game\AdventureQuest Worlds Infinity_Data\Managed"
```

The runner patches disposable copies only. It accepts an already-patched client
and checks the current client rewrite, an exact-version backup, repeat launches,
missing dependencies, a locked destination, and staged-file cleanup.

The missing AudioModule case reproduces the October 2026 failure: Cecil resolved
an enum parameter while writing metadata, threw an AssemblyResolutionException,
and the old direct-to-file writer left Assembly-CSharp.dll empty. The repaired
patcher resolves dependencies from Managed, validates its output in memory, and
atomically replaces the installed file only after successful serialization.
