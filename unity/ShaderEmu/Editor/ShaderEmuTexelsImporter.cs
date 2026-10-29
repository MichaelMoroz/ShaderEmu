using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

// A file of 16-byte texels (the ROM, build/images/nommu/rootfs.bin, copied in as NAME_rom.texels) as one RGBAFloat
// texture: 8192 texels a row, texel t at (t % 8192, t / 8192), every byte as it is, NaNs included; the shaders read a texel
// back with asuint (docs/machine/gpu.md, "The ROM as one texture"). It is made in the import's own binary cache: the
// project saves its assets as text, and the ROM is some 280 MB. (A UInt texture cannot be a Texture2D in Unity 2022.3.)
[ScriptedImporter(2, "texels")]   // (2: rows of 8,192 from row 0; 1 was rows of 2,048, texel 0 in the last row)
public class ShaderEmuTexelsImporter : ScriptedImporter
{
    public const int Width = 8192;
    public bool readable;   // (a check reads the pixels back: docs/machine/gpu.md)

    public override void OnImportAsset(AssetImportContext ctx)
    {
        byte[] bin = File.ReadAllBytes(ctx.assetPath);
        int texels = (bin.Length + 15) / 16, height = Mathf.Max((texels + Width - 1) / Width, 1), pitch = Width * 16;
        byte[] raw = new byte[(long)height * pitch];
        System.Buffer.BlockCopy(bin, 0, raw, 0, bin.Length);
        Texture2D tex = new Texture2D(Width, height, TextureFormat.RGBAFloat, false, true);
        tex.filterMode = FilterMode.Point;
        tex.wrapMode = TextureWrapMode.Clamp;
        tex.SetPixelData(raw, 0);
        tex.Apply(false, !readable);
        ctx.AddObjectToAsset("texels", tex);
        ctx.SetMainObject(tex);
    }

    // The texture's bytes as the file has them: the check that every byte came through (NaNs, denormals, -0).
    public static int Mismatches(Texture2D tex, byte[] bin)
    {
        byte[] raw = tex.GetPixelData<byte>(0).ToArray();
        int height = tex.height, pitch = Width * 16, wrong = 0;
        for (long i = 0; i < bin.Length; i++)
        {
            long y = i / pitch, x = i % pitch;
            if (raw[y * pitch + x] != bin[i]) wrong++;
        }
        return wrong;
    }
}
