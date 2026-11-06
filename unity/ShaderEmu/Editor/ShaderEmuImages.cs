using System.IO;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEngine;

// Brings the machine's shader sources and boot images from the ShaderEmu repository into this
// project. A memory image becomes four PNGs, one per word of a 16-byte texel (r, g, b, a), 2048
// texels a row, first row at the bottom: what rvc's importer made and the shaders read.
public static class ShaderEmuImages
{
    public const string Root = "Assets/ShaderEmu";
    public const string ImageFolder = Root + "/Images";

    public class BootImage
    {
        public string name, title, ram, rom, kernel, tree;
    }

    public static readonly BootImage[] Images =
    {
        // Linux without an MMU (docs/system/nommu.md), for MachineTickLean.shader's machine.
        new BootImage { name = "linux", title = "Linux: shell, Nano-X desktop (nx), Doom, glxgears",
                        ram = "build/images/nommu/linux_payload.bin", rom = "build/images/nommu/rootfs.bin",
                        kernel = "build/images/nommu/Image", tree = "build/images/nommu/dts.bin" },
        // The bare-metal programs (programs/bin: gears, raycast, raytrace, blend, rects) start in
        // machine mode at 0x80000000, which the SBI_HLE build of the shader does not do.
    };

    public static string Repo
    {
        get { return EditorPrefs.GetString("ShaderEmu.Repo", "C:/Development/ShaderEmu"); }
    }

    public static string LanePath(string name, string part, int lane)
    {
        return ImageFolder + "/" + name + "_" + part + "." + "rgba"[lane] + ".png";
    }

    // The ROM as one texture (ShaderEmuTexelsImporter): the image's file as it is.
    public static string RomPath(string name)
    {
        return ImageFolder + "/" + name + "_rom.texels";
    }

    // The world keeps no image of RAM (AGENTS.md, 5 November 2026). The ROM's texture holds, past the root file
    // system, what a boot loader puts in RAM: the kernel, then its device tree, each from a row of its own. The
    // reset copies them (src/ram.cginc, RAM_BOOT_FROM_ROM). Where each goes in RAM: tools/make_linux_image.py and
    // linux/nommu/image.py (the kernel at 0x82000000, its tree at 0x83e00000; texels from 0x80000000).
    public const int KernelAt = 0x2000000 / 16, TreeAt = 0x3e00000 / 16, Row = 2048;

    static int Rows(long bytes) { return (int)((bytes + Row * 16 - 1) / (Row * 16)); }

    // (first texel of RAM, texels, first texel of the ROM's texture) for the kernel and the tree, and where the digit of
    // the command line's loglevel= lies (its texel of RAM, its byte in that texel); false without the files
    public static bool BootPieces(BootImage image, out Vector4 kernel, out Vector4 tree, out Vector4 log)
    {
        kernel = tree = log = Vector4.zero;
        string rom = Repo + "/" + image.rom, k = Repo + "/" + image.kernel, t = Repo + "/" + image.tree;
        if (!File.Exists(rom) || !File.Exists(k) || !File.Exists(t)) return false;
        int digit;
        long kb = new FileInfo(k).Length, tb = WithLogLevel(File.ReadAllBytes(t), out digit).Length;
        int kAt = Rows(new FileInfo(rom).Length) * Row, tAt = kAt + Rows(kb) * Row;
        kernel = new Vector4(KernelAt, (kb + 15) / 16, kAt, 0);
        tree = new Vector4(TreeAt, (tb + 15) / 16, tAt, 0);
        log = new Vector4(TreeAt + digit / 16, digit % 16, 0, 0);
        return true;
    }

    // The device tree with "loglevel=4" on the kernel's command line (bootargs), and where its digit is: the reset
    // writes the level chosen on the panel there (EmuMachine.logLevel), so the kernel's console prints that much of
    // its log. 4 is the kernel's own default (linux/nommu/kernel.sh, CONFIG_CONSOLE_LOGLEVEL_DEFAULT): errors only.
    // The property grows, so the struct block's size, the total and every block after it move (as image.py's with_seed).
    public static byte[] WithLogLevel(byte[] dtb, out int digit)
    {
        System.Func<byte[], int, int> be = (b, i) => (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3];
        System.Action<byte[], int, int> put = (b, i, v) => { b[i] = (byte)(v >> 24); b[i + 1] = (byte)(v >> 16); b[i + 2] = (byte)(v >> 8); b[i + 3] = (byte)v; };
        if (be(dtb, 0) != unchecked((int)0xd00dfeed)) throw new System.Exception("the device tree has no FDT header");
        int structAt = be(dtb, 8), stringsAt = be(dtb, 12), at = structAt;
        while (true)
        {
            int token = be(dtb, at);
            if (token == 1) { at += 4; while (dtb[at] != 0) at++; at = (at + 4) & ~3; continue; }   // a node's name, NUL, padded
            if (token == 2 || token == 4) { at += 4; continue; }
            if (token != 3) throw new System.Exception("the device tree has no bootargs");
            int length = be(dtb, at + 4), name = be(dtb, at + 8), n = stringsAt + name, value = at + 12;
            string prop = "";
            while (dtb[n] != 0) prop += (char)dtb[n++];
            if (prop == "bootargs")
            {
                string args = System.Text.Encoding.ASCII.GetString(dtb, value, length).TrimEnd('\0');
                int was = args.IndexOf("loglevel=");
                if (was >= 0) { digit = value + was + 9; return dtb; }
                byte[] text = System.Text.Encoding.ASCII.GetBytes(args + " loglevel=4\0");
                int oldPadded = (length + 3) & ~3, newPadded = (text.Length + 3) & ~3, grow = newPadded - oldPadded;
                byte[] o = new byte[dtb.Length + grow];
                System.Array.Copy(dtb, o, value);
                System.Array.Copy(text, 0, o, value, text.Length);
                System.Array.Copy(dtb, value + oldPadded, o, value + newPadded, dtb.Length - value - oldPadded);
                put(o, at + 4, text.Length);
                put(o, 4, be(dtb, 4) + grow);                            // totalsize
                for (int h = 8; h <= 16; h += 4) if (be(dtb, h) > at) put(o, h, be(dtb, h) + grow);   // the blocks after it
                put(o, 36, be(dtb, 36) + grow);                          // size_dt_struct
                digit = value + text.Length - 2;
                return o;
            }
            at = value + ((length + 3) & ~3);
        }
    }

    [MenuItem("ShaderEmu/Sync shader sources from the repository")]
    public static void SyncShaders()
    {
        string from = Repo + "/shader_vm", to = Root + "/Shaders";
        foreach (string file in new[] { "crt.cginc", "helpers.cginc" }) WriteIfChanged(to + "/" + file, File.ReadAllText(from + "/" + file));
        Directory.CreateDirectory(to + "/src");
        // As .cginc: Unity tracks those as shader includes; a changed .h was never recompiled.
        foreach (string path in Directory.GetFiles(from + "/src", "*.h"))
        {
            string text = Regex.Replace(File.ReadAllText(path), "(#include\\s+\"[^\"]+)\\.h\"", "$1.cginc\"");
            WriteIfChanged(to + "/src/" + Path.GetFileNameWithoutExtension(path) + ".cginc", text);
        }
        AssetDatabase.Refresh();
        Debug.Log("[ShaderEmu] shader sources copied from " + from);
    }

    // An untouched file keeps its date, and Unity does not compile the shaders that include it again.
    static void WriteIfChanged(string path, string text)
    {
        if (File.Exists(path) && File.ReadAllText(path) == text) return;
        File.WriteAllText(path, text);
    }

    [MenuItem("ShaderEmu/Import boot images from the repository")]
    public static void ImportImages()
    {
        Directory.CreateDirectory(ImageFolder);
        foreach (BootImage image in Images)
        {
            WriteRom(image);
            // (an image of RAM from before the rule goes)
            string old = ImageFolder + "/" + image.name + "_ram.texels";
            if (File.Exists(old)) AssetDatabase.DeleteAsset(old);
        }
        AssetDatabase.Refresh();
        // the machine in the open scene copies the kernel and its tree from where they now lie: past a root file system of
        // another size they moved, and a scene that kept the old places booted nothing (6 November 2026)
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>(true);
        if (machine != null && !Application.isPlaying)
        {
            UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(machine);
            BootPieces(Images[0], out machine.bootKernel, out machine.bootTree, out machine.bootLog);
            UdonSharpEditor.UdonSharpEditorUtility.CopyProxyToUdon(machine);
            EditorUtility.SetDirty(machine);
            UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        }
        Debug.Log("[ShaderEmu] boot images imported from " + Repo + (machine != null ? "; the scene's machine boots from the kernel at texel " + machine.bootKernel.z : ""));
    }

    // The ROM's texels: the root file system, then the kernel and its tree, each padded to whole rows
    static void WriteRom(BootImage image)
    {
        Vector4 kernel, tree, log;
        if (!BootPieces(image, out kernel, out tree, out log))
        {
            Debug.LogError("[ShaderEmu] " + Repo + "/" + image.rom + ", " + image.kernel + " or " + image.tree + " is missing: no ROM for '" + image.name + "'");
            return;
        }
        int digit;
        byte[] k = File.ReadAllBytes(Repo + "/" + image.kernel), t = WithLogLevel(File.ReadAllBytes(Repo + "/" + image.tree), out digit);
        using (FileStream rom = File.OpenRead(Repo + "/" + image.rom))
        using (FileStream to = File.Create(RomPath(image.name)))
        {
            rom.CopyTo(to);
            to.SetLength((long)kernel.z * 16);
            to.Position = (long)kernel.z * 16;
            to.Write(k, 0, k.Length);
            to.SetLength((long)tree.z * 16);
            to.Position = (long)tree.z * 16;
            to.Write(t, 0, t.Length);
        }
    }

    // RAM's image and the ROM in four lanes as before, for the Udon model only (EmuUdonCpu reads them on the processor).
    [MenuItem("ShaderEmu/Import the ROM's lanes for the Udon model")]
    public static void ImportRomLanes()
    {
        Directory.CreateDirectory(ImageFolder);
        foreach (BootImage image in Images)
        {
            WriteLanes(image.name, "ram", image.ram);
            WriteLanes(image.name, "rom", image.rom);
        }
        AssetDatabase.Refresh();
    }

    static void WriteLanes(string name, string part, string file)
    {
        if (string.IsNullOrEmpty(file)) return;
        string path = Repo + "/" + file;
        if (!File.Exists(path))
        {
            Debug.LogWarning("[ShaderEmu] " + path + " is missing; image '" + name + "' will lack its " + part);
            return;
        }
        byte[] bin = File.ReadAllBytes(path);
        int texels = (bin.Length + 15) / 16;
        int width = 2048, height = Mathf.Max((texels + 2047) / 2048, 1);
        for (int lane = 0; lane < 4; lane++)
        {
            byte[] raw = new byte[width * height * 4];
            for (int t = 0; t < texels; t++)
            {
                // texel 0 is in the texture's last row
                int at = ((height - 1 - t / width) * width + t % width) * 4, src = t * 16 + lane * 4;
                for (int k = 0; k < 4 && src + k < bin.Length; k++) raw[at + k] = bin[src + k];
            }
            Texture2D tex = new Texture2D(width, height, TextureFormat.RGBA32, false, true);
            tex.LoadRawTextureData(raw);
            File.WriteAllBytes(LanePath(name, part, lane), tex.EncodeToPNG());
            Object.DestroyImmediate(tex);
        }
    }
}

// Data textures must reach the shader byte for byte.
public class ShaderEmuTextureImport : AssetPostprocessor
{
    void OnPreprocessTexture()
    {
        TextureImporter importer = (TextureImporter)assetImporter;
        bool data = assetPath.StartsWith(ShaderEmuImages.ImageFolder + "/");
        bool font = assetPath == ShaderEmuImages.Root + "/Textures/TerminalFont.png";
        // sharper at a distance than the default box filter, which the SDK's panel also asks to be rid of
        if (assetPath.StartsWith(ShaderEmuImages.Root + "/") && !data) importer.mipmapFilter = TextureImporterMipFilter.KaiserFilter;
        if (!data && !font) return;
        importer.textureType = TextureImporterType.Default;
        importer.sRGBTexture = false;
        importer.npotScale = TextureImporterNPOTScale.None;
        importer.textureCompression = TextureImporterCompression.Uncompressed;
        importer.maxTextureSize = 16384;
        importer.wrapMode = TextureWrapMode.Clamp;
        importer.alphaIsTransparency = false;
        if (data)
        {
            importer.mipmapEnabled = false;
            importer.filterMode = FilterMode.Point;
            importer.alphaSource = TextureImporterAlphaSource.FromInput;
            TextureImporterPlatformSettings settings = importer.GetDefaultPlatformTextureSettings();
            settings.format = TextureImporterFormat.RGBA32;
            settings.maxTextureSize = 16384;
            settings.textureCompression = TextureImporterCompression.Uncompressed;
            importer.SetPlatformTextureSettings(settings);
        }
        else
        {
            importer.mipmapEnabled = true;
            importer.filterMode = FilterMode.Trilinear;
            importer.alphaSource = TextureImporterAlphaSource.None;
        }
    }
}
