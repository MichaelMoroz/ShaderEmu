using System.IO;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEngine;

// Brings the machine's shader sources and boot images from the ShaderX86 repository into this
// project. A memory image becomes four PNGs, one per word of a 16-byte texel (r, g, b, a), 2048
// texels a row, first row at the bottom: what rvc's importer made and the shaders read.
public static class ShaderEmuImages
{
    public const string Root = "Assets/ShaderEmu";
    public const string ImageFolder = Root + "/Images";

    public class BootImage
    {
        public string name, title, ram, rom, tree;
    }

    public static readonly BootImage[] Images =
    {
        new BootImage { name = "linux", title = "Linux: shell, Nano-X desktop (nx), Doom, glxgears",
                        ram = "build/images/linux/linux_payload.bin", rom = "build/images/linux/rootfs.bin",
                        tree = "build/images/linux/dts.bin" },
        // The bare-metal programs (programs/bin: gears, raycast, raytrace, blend, rects) start in
        // machine mode at 0x80000000, which the SBI_HLE build of the shader does not do.
    };

    public static string Repo
    {
        get { return EditorPrefs.GetString("ShaderEmu.Repo", "C:/Development/ShaderX86"); }
    }

    public static string LanePath(string name, string part, int lane)
    {
        return ImageFolder + "/" + name + "_" + part + "." + "rgba"[lane] + ".png";
    }

    [MenuItem("ShaderEmu/Sync shader sources from the repository")]
    public static void SyncShaders()
    {
        string from = Repo + "/experiments/rvc_opt", to = Root + "/Shaders";
        foreach (string file in new[] { "crt.cginc", "helpers.cginc" }) File.Copy(from + "/" + file, to + "/" + file, true);
        Directory.CreateDirectory(to + "/src");
        // As .cginc: Unity tracks those as shader includes; a changed .h was never recompiled.
        foreach (string path in Directory.GetFiles(from + "/src", "*.h"))
        {
            string text = Regex.Replace(File.ReadAllText(path), "(#include\\s+\"[^\"]+)\\.h\"", "$1.cginc\"");
            File.WriteAllText(to + "/src/" + Path.GetFileNameWithoutExtension(path) + ".cginc", text);
        }
        AssetDatabase.Refresh();
        Debug.Log("[ShaderEmu] shader sources copied from " + from);
    }

    [MenuItem("ShaderEmu/Import boot images from the repository")]
    public static void ImportImages()
    {
        Directory.CreateDirectory(ImageFolder);
        foreach (BootImage image in Images)
        {
            WriteLanes(image.name, "ram", image.ram);
            WriteLanes(image.name, "rom", image.rom);
            WriteLanes(image.name, "tree", image.tree);
        }
        AssetDatabase.Refresh();
        Debug.Log("[ShaderEmu] boot images imported from " + Repo);
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
        // sharper at a distance than the default box filter
        if (assetPath.StartsWith(ShaderEmuImages.Root + "/Textures/")) importer.mipmapFilter = TextureImporterMipFilter.KaiserFilter;
        if (!data && !font) return;
        importer.textureType = TextureImporterType.Default;
        importer.sRGBTexture = false;
        importer.npotScale = TextureImporterNPOTScale.None;
        importer.textureCompression = TextureImporterCompression.Uncompressed;
        importer.maxTextureSize = 8192;
        importer.wrapMode = TextureWrapMode.Clamp;
        importer.alphaIsTransparency = false;
        if (data)
        {
            importer.mipmapEnabled = false;
            importer.filterMode = FilterMode.Point;
            importer.alphaSource = TextureImporterAlphaSource.FromInput;
            TextureImporterPlatformSettings settings = importer.GetDefaultPlatformTextureSettings();
            settings.format = TextureImporterFormat.RGBA32;
            settings.maxTextureSize = 8192;
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
