using System.IO;
using System.Text.RegularExpressions;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEngine;
using UnityEngine.Experimental.Rendering;

// This copy of the world runs GPU-VM (gpu_vm/platform.md in the custom_isa worktree), as rvc_harness --gvm does
// (harness/apps/gvm_backend11.cpp): its boot image as textures, and the machine's textures, meshes and cameras.
public static partial class ShaderEmuBuilder
{
    const string GvmShaders = Root + "/Shaders/gvm";

    static string GvmImageDir
    {
        get { return EditorPrefs.GetString("ShaderEmu.GvmImage", "C:/Development/ShaderX86-custom_isa/build/gvm_apps/img-cur"); }
    }

    // unity_ram.bin (gpu_vm/platform/unity_image.py: RAM from address 0 as the harness starts it) and rootfs.romfs, as
    // four PNGs each (a word of a texel each, 2048 texels a row, texel 0 in the last row: ShaderEmuImages' form).
    [MenuItem("ShaderEmu/GPU-VM: import the boot image")]
    public static void GvmImportImage()
    {
        var watch = System.Diagnostics.Stopwatch.StartNew();
        Directory.CreateDirectory(ShaderEmuImages.ImageFolder);
        GvmLanes("ram", GvmImageDir + "/unity_ram.bin");
        GvmLanes("rom", GvmImageDir + "/rootfs.romfs");
        AssetDatabase.Refresh();
        Debug.Log("[GVM] boot image imported from " + GvmImageDir + " in " + watch.Elapsed.TotalSeconds.ToString("F1") + " s");
    }

    static void GvmLanes(string part, string path)
    {
        byte[] bin = File.ReadAllBytes(path);
        int texels = (bin.Length + 15) / 16, width = 2048, height = Mathf.Max((texels + width - 1) / width, 1);
        for (int lane = 0; lane < 4; lane++)
        {
            byte[] raw = new byte[width * height * 4];
            for (int t = 0; t < texels; t++)
            {
                int at = ((height - 1 - t / width) * width + t % width) * 4, src = t * 16 + lane * 4;
                for (int k = 0; k < 4 && src + k < bin.Length; k++) raw[at + k] = bin[src + k];
            }
            Texture2D tex = new Texture2D(width, height, TextureFormat.RGBA32, false, true);
            tex.LoadRawTextureData(raw);
            File.WriteAllBytes(ShaderEmuImages.LanePath("gvm", part, lane), tex.EncodeToPNG());
            Object.DestroyImmediate(tex);
        }
    }

    static Mesh GvmMesh(string name, int count, MeshTopology topology)
    {
        Mesh mesh = LoadOrCreate(Generated + "/" + name + ".asset", () => new Mesh { name = name });
        if (mesh.vertexCount != count)
        {
            int[] indices = new int[count];
            for (int i = 0; i < count; i++) indices[i] = i;
            mesh.Clear();
            mesh.indexFormat = UnityEngine.Rendering.IndexFormat.UInt32;
            mesh.vertices = new Vector3[count];
            mesh.SetIndices(indices, topology, 0, false);
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one);
            EditorUtility.SetDirty(mesh);
        }
        return mesh;
    }

    static int GvmConst(string source, string name, int none = -1)
    {
        if (none >= 0 && !Regex.IsMatch(source, "static const uint " + name + " = (\\d+);")) return none;
        Match m = Regex.Match(source, "static const uint " + name + " = (\\d+);");
        if (!m.Success) throw new System.Exception("tick.cginc has no " + name);
        return int.Parse(m.Groups[1].Value);
    }

    // The machine on the RISC-V world's objects: the tick camera draws GvmTick's mesh into a state set; the points
    // camera, the commit's and the operations' meshes, one at a time; the GPU and the sound card read gpu_vm's RAM.
    [MenuItem("ShaderEmu/GPU-VM: build the machine")]
    public static void GvmBuild()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        EmuMachine machine = world.GetComponentInChildren<EmuMachine>(true);
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        string tick = File.ReadAllText(GvmShaders + "/tick.cginc");
        // c0nb: core 0's own write cache of WCN0 entries and its own tick (GvmTick0.shader); 0 for a step without
        int wcn0 = GvmConst(tick, "WCN0", 0);
        int wcn = GvmConst(tick, "WCN"), slots = GvmConst(tick, "OP_SLOTS"), span = GvmConst(tick, "OP_SPAN");
        string[] boot = File.ReadAllLines(GvmImageDir + "/unity_boot.txt");
        machine.gvmEntry = System.Convert.ToInt32(boot[0].Split(' ')[1], 16);
        machine.gvmRamMB = int.Parse(boot[1].Split(' ')[1]);
        if (GvmConst(tick, "GEO_BW") != 8 || GvmConst(tick, "GEO_BAND") != 16) throw new System.Exception("tick.cginc: blocks 8 wide, bands of 16 rows");
        machine.gvmBlockH = GvmConst(tick, "GEO_BH");
        machine.gvmStateTexels = GvmConst(tick, "NTEX");
        long romBytes = new FileInfo(GvmImageDir + "/rootfs.romfs").Length;

        machine.state = StateTexture("GvmRam", 8192, (0x2000000 + (machine.gvmRamMB << 20)) / 16 / 8192);
        machine.gvmRom = StateTexture("GvmRom", 8192, (int)((romBytes + 16 * 8192 - 1) / (16 * 8192)));
        machine.gvmSetA = new RenderTexture[8];
        machine.gvmSetB = new RenderTexture[8];
        for (int t = 0; t < 8; t++)
        {
            machine.gvmSetA[t] = StateTexture("GvmStateA" + t, 1024, 24);   // GEO_BLOCKS: 128 cores' blocks a band of 16 rows
            machine.gvmSetB[t] = StateTexture("GvmStateB" + t, 1024, 24);
        }
        machine.gvmOps = new[] { StateTexture("GvmOps0", 256, 1), StateTexture("GvmOps1", 256, 1) };
        machine.gvmStage = StateTexture("GvmStage", span, 256 * slots);
        machine.gvmPlan = StateTexture("GvmPlan", 2 * slots, 256);
        machine.gvmPending = StateTexture("GvmPending", 1, 1);
        machine.gvmZone = StateTexture("GvmZone", 8192, 3);
        machine.gvmCtl = StateTexture("GvmCtl", 256, 1);
        machine.gvmTickDepth = Target("GvmTickDepth", 1024, 24, 16);

        Material tickMat = Mat("GvmTick", "ShaderEmu/GvmTick");
        machine.tickMaterial = tickMat;
        machine.machineMaterial = Mat("GvmMachine", "ShaderEmu/GvmMachine");
        machine.gvmCommitMaterial = Mat("GvmCommit", "ShaderEmu/GvmCommit");
        machine.gvmStageMaterial = Mat("GvmOpStage", "ShaderEmu/GvmOps");
        machine.gvmWriteMaterial = Mat("GvmOpWrite", "ShaderEmu/GvmOps");
        machine.gvmProgMaterial = Mat("GvmOpProg", "ShaderEmu/GvmOps");
        machine.gvmWritebackMaterial = Mat("GvmWriteback", "ShaderEmu/GvmWriteback");
        machine.gvmOpsBlitMaterial = Mat("GvmOpsBlit", "ShaderEmu/GvmOpsBlit");
        string[] draws = { "GVM_STAGE", "GVM_WRITE", "GVM_PROG" };
        Material[] ops = { machine.gvmStageMaterial, machine.gvmWriteMaterial, machine.gvmProgMaterial };
        for (int i = 0; i < 3; i++)
        {
            foreach (string k in draws) ops[i].DisableKeyword(k);
            ops[i].EnableKeyword(draws[i]);
        }
        machine.gpuMaterial.shader = Shader.Find("ShaderEmu/GvmGpuDraw");
        if (machine.sound != null) machine.sound.mixMaterial.shader = Shader.Find("ShaderEmu/GvmSound");

        // the tick's camera and mesh: a quad a core
        Transform device = world.Find("GPU device");
        MeshFilter tickMesh = device.Find("Tick mesh").GetComponent<MeshFilter>();
        tickMesh.sharedMesh = GvmMesh("GvmTickQuads", 6 * MachineCores, MeshTopology.Triangles);
        tickMesh.GetComponent<MeshRenderer>().sharedMaterial = tickMat;
        Transform oldTick0 = device.Find("GvmTick0 mesh");
        if (oldTick0 != null) Object.DestroyImmediate(oldTick0.gameObject);
        machine.gvmTick0Material = null;
        if (wcn0 > 0)
        {
            machine.gvmTick0Material = Mat("GvmTick0", "ShaderEmu/GvmTick0");
            MeshRenderer tick0 = PassMesh(device, "GvmTick0", tickMesh.gameObject.layer, GvmMesh("GvmTick0Quad", 6, MeshTopology.Triangles), machine.gvmTick0Material);
            tick0.transform.localPosition = tickMesh.transform.localPosition;
            tick0.transform.localRotation = tickMesh.transform.localRotation;
            tick0.transform.localScale = tickMesh.transform.localScale;
        }
        // the points camera draws one of these at a time; the RISC-V machine's meshes are off
        machine.gvmPassCamera = machine.pointsCamera;
        machine.pointsRenderer.enabled = false;
        machine.gatherRenderer.enabled = false;
        foreach (string old in new[] { "GvmCommit mesh", "GvmStage mesh", "GvmWrite mesh", "GvmProg mesh" })
        {
            Transform t = device.Find(old);
            if (t != null) Object.DestroyImmediate(t.gameObject);
        }
        machine.gvmCommitRenderer = PassMesh(device, "GvmCommit", PointsLayer, GvmMesh("GvmCommitPoints", wcn0 > 0 ? wcn0 + (MachineCores - 1) * wcn : MachineCores * wcn, MeshTopology.Points), machine.gvmCommitMaterial);
        machine.gvmStageRenderer = PassMesh(device, "GvmStage", PointsLayer, GvmMesh("GvmStageQuads", 6 * MachineCores * slots, MeshTopology.Triangles), machine.gvmStageMaterial);
        machine.gvmWriteRenderer = PassMesh(device, "GvmWrite", PointsLayer, GvmMesh("GvmWriteQuads", 12 * MachineCores * slots, MeshTopology.Triangles), machine.gvmWriteMaterial);
        machine.gvmProgRenderer = PassMesh(device, "GvmProg", PointsLayer, GvmMesh("GvmProgPoints", MachineCores, MeshTopology.Points), machine.gvmProgMaterial);
        foreach (MeshRenderer r in new[] { machine.gvmCommitRenderer, machine.gvmStageRenderer, machine.gvmWriteRenderer, machine.gvmProgRenderer })
            r.enabled = false;
        // a pass renders four times: nothing for the cameras to cull or light but the one mesh
        foreach (MeshRenderer r in new[] { machine.gvmCommitRenderer, machine.gvmStageRenderer, machine.gvmWriteRenderer, machine.gvmProgRenderer,
                                           tickMesh.GetComponent<MeshRenderer>() })
        {
            r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
            r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
        }
        foreach (Camera cam in new[] { machine.tickCamera, machine.pointsCamera })
        {
            cam.useOcclusionCulling = false;
            cam.allowHDR = false;
            cam.allowMSAA = false;
        }

        // the boot image: RAM's four textures, then the ROM's
        machine.imageTextures = new Texture2D[8];
        for (int lane = 0; lane < 4; lane++)
        {
            machine.imageTextures[lane] = AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath("gvm", "ram", lane));
            machine.imageTextures[4 + lane] = AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath("gvm", "rom", lane));
        }
        Apply(machine);
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        AssetDatabase.SaveAssets();
        Debug.Log("[GVM] the machine is built: RAM " + machine.state.width + " x " + machine.state.height + ", ROM " + machine.gvmRom.height + " rows, WCN " + wcn + (wcn0 > 0 ? ", core 0's WCN0 " + wcn0 : ""));
    }
}

