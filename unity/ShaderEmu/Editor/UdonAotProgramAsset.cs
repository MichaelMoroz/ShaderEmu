using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using VRC.Udon;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.Editor.ProgramSources;

// tools/udon_aot/linux2udon.py's output as an Udon program: its bytecode and heap as the
// generator wrote them, with no assembler (the kernel is 50 MB of bytecode). EmuUdonCpu runs it.
public class UdonAotProgramAsset : UdonProgramAsset
{
    public string sourceDir = "build/udon_linux";   // in the repository

    protected override void RefreshProgramImpl()
    {
        string dir = ShaderEmuImages.Repo + "/" + sourceDir;
        if (File.Exists(dir + "/prog.bc")) program = Build(dir);
        else if (program == null && serializedUdonProgramAsset != null) program = serializedUdonProgramAsset.RetrieveProgram();
    }

    // the base class would disassemble 50 MB into the inspector
    protected override void DrawProgramSourceGUI(UdonBehaviour udonBehaviour, ref bool dirty)
    {
        EditorGUILayout.LabelField("Translated kernel: " + sourceDir);
        if (program != null) EditorGUILayout.LabelField(string.Format("{0:N0} bytes of bytecode", program.ByteCode.Length));
    }

    static readonly Type[] Types = { typeof(int), typeof(uint), typeof(bool), typeof(long), typeof(string), typeof(int[]), typeof(uint[]), typeof(byte), typeof(float) };

    public static IUdonProgram Build(string dir)
    {
        var externs = File.ReadAllLines(dir + "/prog.externs");
        var hb = File.ReadAllBytes(dir + "/prog.heap");
        int n = BitConverter.ToInt32(hb, 0);
        var heap = new UdonHeap((uint)n + 16);
        var types = new Type[n];
        for (int i = 0; i < n; i++)
        {
            int at = 4 + i * 9; byte typ = hb[at]; long v = BitConverter.ToInt64(hb, at + 1);
            uint slot = (uint)i;
            types[i] = Types[typ];
            heap.InitializeHeapVariable(slot, types[i]);
            switch (typ)
            {
                case 0: heap.SetHeapVariable(slot, (int)v, typeof(int)); break;
                case 1: heap.SetHeapVariable(slot, (uint)v, typeof(uint)); break;
                case 2: heap.SetHeapVariable(slot, v != 0, typeof(bool)); break;
                case 3: heap.SetHeapVariable(slot, v, typeof(long)); break;
                case 4: heap.SetHeapVariable(slot, externs[v], typeof(string)); break;
                case 7: heap.SetHeapVariable(slot, (byte)v, typeof(byte)); break;
                case 8: heap.SetHeapVariable(slot, BitConverter.Int32BitsToSingle((int)v), typeof(float)); break;
            }
        }
        var syms = new Dictionary<string, uint>();
        foreach (var line in File.ReadAllLines(dir + "/prog.syms"))
        {
            var p = line.Split(' ');
            if (p.Length == 2) syms[p[0]] = uint.Parse(p[1]);
        }
        // the tables, the constants that have no literal, and the state at reset
        foreach (var f in Directory.GetFiles(dir, "*.i32"))
        {
            string name = Path.GetFileNameWithoutExtension(f);
            if (!syms.ContainsKey(name)) continue;
            var b = File.ReadAllBytes(f); var a = new int[b.Length / 4]; Buffer.BlockCopy(b, 0, a, 0, b.Length);
            heap.SetHeapVariable(syms[name], a, typeof(int[]));
        }
        foreach (var f in Directory.GetFiles(dir, "*.u32"))
        {
            string name = Path.GetFileNameWithoutExtension(f);
            if (!syms.ContainsKey(name)) continue;
            var b = File.ReadAllBytes(f); var a = new uint[b.Length / 4]; Buffer.BlockCopy(b, 0, a, 0, b.Length);
            heap.SetHeapVariable(syms[name], a, typeof(uint[]));
        }
        heap.SetHeapVariable(syms["k_u32"], 0xFFFFFFFFL, typeof(long));
        heap.SetHeapVariable(syms["k_l0"], 0L, typeof(long));

        var symbols = new List<IUdonSymbol>();
        foreach (var kv in syms)
            if (kv.Key[0] != '@') symbols.Add(new UdonSymbol(kv.Key, types[kv.Value], kv.Value));
        var entry = new List<IUdonSymbol> { new UdonSymbol("Slice", null, syms["@entry"]) };
        return new UdonProgram("UDON", 1, File.ReadAllBytes(dir + "/prog.bc"), heap,
            new UdonSymbolTable(entry, new[] { "Slice" }), new UdonSymbolTable(symbols, new string[0]),
            new UdonSyncMetadataTable(), 0);
    }
}
