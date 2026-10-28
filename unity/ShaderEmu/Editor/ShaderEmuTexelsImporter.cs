using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

// A file of 16-byte texels (the ROM, build/images/nommu/rootfs.bin, copied in as NAME_rom.texels) as one RGBAFloat
// texture: 2048 texels a row, texel 0 in the last row, every byte as it is, NaNs included; the shaders read a texel
// back with asuint (docs/machine/gpu.md, "The ROM as one texture"). It is made in the import's own binary cache: the
// project saves its assets as text, and the ROM is some 280 MB. (A UInt texture cannot be a Texture2D in Unity 2022.3.)
[ScriptedImporter(1, "texels")]
public class ShaderEmuTexelsImporter : ScriptedImporter
{
    public const int Width = 2048;
    public bool readable;   // (a check reads the pixels back: docs/machine/gpu.md)

    public override void OnImportAsset(AssetImportContext ctx)
    {
        byte[] bin = File.ReadAllBytes(ctx.assetPath);
        int texels = (bin.Length + 15) / 16, height = Mathf.Max((texels + Width - 1) / Width, 1), pitch = Width * 16;
        byte[] raw = new byte[(long)height * pitch];
        for (int y = 0; y < height; y++)
        {
            long from = (long)y * pitch;
            if (from >= bin.Length) break;
            System.Buffer.BlockCopy(bin, (int)from, raw, (height - 1 - y) * pitch, (int)System.Math.Min(pitch, bin.Length - from));
        }
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
            if (raw[(height - 1 - y) * pitch + x] != bin[i]) wrong++;
        }
        return wrong;
    }
}
