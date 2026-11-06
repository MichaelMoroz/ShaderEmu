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

    // (first texel of RAM, texels, first texel of the ROM's texture) for the kernel and the tree; false without the files
    public static bool BootPieces(BootImage image, out Vector4 kernel, out Vector4 tree)
    {
        kernel = tree = Vector4.zero;
        string rom = Repo + "/" + image.rom, k = Repo + "/" + image.kernel, t = Repo + "/" + image.tree;
        if (!File.Exists(rom) || !File.Exists(k) || !File.Exists(t)) return false;
        long kb = new FileInfo(k).Length, tb = new FileInfo(t).Length;
        int kAt = Rows(new FileInfo(rom).Length) * Row, tAt = kAt + Rows(kb) * Row;
        kernel = new Vector4(KernelAt, (kb + 15) / 16, kAt, 0);
        tree = new Vector4(TreeAt, (tb + 15) / 16, tAt, 0);
        return true;
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
        Debug.Log("[ShaderEmu] boot images imported from " + Repo);
    }

    // The ROM's texels: the root file system, then the kernel and its tree, each padded to whole rows
    static void WriteRom(BootImage image)
    {
        Vector4 kernel, tree;
        if (!BootPieces(image, out kernel, out tree))
        {
            Debug.LogError("[ShaderEmu] " + Repo + "/" + image.rom + ", " + image.kernel + " or " + image.tree + " is missing: no ROM for '" + image.name + "'");
            return;
        }
        byte[] k = File.ReadAllBytes(Repo + "/" + image.kernel), t = File.ReadAllBytes(Repo + "/" + image.tree);
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
