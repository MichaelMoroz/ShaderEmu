using System.Collections.Generic;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Events;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.UI;

// Beyond the den's two doors (world/annex.py, docs/holodeck.md): the holodeck, where a 3D
// program's frame is a world round the visitor, and the corridor a visitor arrives in.
// The models are the annex's; this gives them their colliders, lights and controls.
public static partial class ShaderEmuBuilder
{
    // as world/room.py and world/annex.py
    const float HoloZ = 3.9f, HoloWide = 1.3f, HoloHigh = 2.3f, HoloSide = 8f, HoloTall = 4f;
    const float CorX0 = -6f, CorX1 = 4.5f, CorWide = 2.4f, CorHigh = 2.8f;
    const float BackDoorX = 3.4f, BackDoorWide = 0.95f, BackDoorHigh = 2.1f;

    static void Solid(Transform parent, string name, float x0, float x1, float y0, float y1, float z0, float z1)
    {
        GameObject go = new GameObject(name);
        go.transform.SetParent(parent, false);
        go.transform.localPosition = new Vector3((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        go.AddComponent<BoxCollider>().size = new Vector3(x1 - x0, y1 - y0, z1 - z0);
        go.isStatic = true;
    }

    static void Gone(Transform parent, string name)
    {
        for (int i = parent.childCount - 1; i >= 0; i--)
            if (parent.GetChild(i).name == name) Object.DestroyImmediate(parent.GetChild(i).gameObject);
    }

    static void Annex(Transform world, Transform models)
    {
        const float halfW = 4.5f, halfD = 5.5f, height = 3.2f, wall = 0.2f;
        Transform room = world.Find("Room"), decor = world.Find("Decor"), computer = world.Find("Computer");
        float holoX1 = -halfW - wall, holoX0 = holoX1 - HoloSide, holoZ0 = HoloZ - HoloSide / 2, holoZ1 = HoloZ + HoloSide / 2;
        float corZ1 = -halfD - wall, corZ0 = corZ1 - CorWide;

        // ---- what stops a visitor: the den's two walls with their doorways, and the new rooms' own
        Gone(room, "Wall left");
        Gone(room, "Wall back");
        foreach (string door in new[] { "Door", "Door frame top", "Door frame side", "Door knob" }) Gone(decor, door);
        Gone(world, "Annex colliders");
        Transform solid = new GameObject("Annex colliders").transform;
        solid.SetParent(world, false);
        float hz0 = HoloZ - HoloWide / 2, hz1 = HoloZ + HoloWide / 2, dx0 = BackDoorX - BackDoorWide / 2, dx1 = BackDoorX + BackDoorWide / 2;
        Solid(solid, "Wall left", -halfW - wall, -halfW, 0, height, -halfD, hz0);
        Solid(solid, "Wall left", -halfW - wall, -halfW, 0, height, hz1, halfD);
        Solid(solid, "Wall left", -halfW - wall, -halfW, HoloHigh, HoloTall, hz0, hz1);
        Solid(solid, "Wall left", -halfW - wall, -halfW, height, HoloTall, holoZ0, halfD);
        Solid(solid, "Wall left", -halfW - wall, -halfW, 0, HoloTall, halfD, holoZ1);
        Solid(solid, "Wall back", CorX0, dx0, 0, height, -halfD - wall, -halfD);
        Solid(solid, "Wall back", dx1, CorX1 + wall, 0, height, -halfD - wall, -halfD);
        Solid(solid, "Wall back", dx0, dx1, BackDoorHigh, height, -halfD - wall, -halfD);
        Solid(solid, "Holodeck floor", holoX0, -halfW, -0.1f, 0, holoZ0, holoZ1);
        Solid(solid, "Holodeck ceiling", holoX0, holoX1, HoloTall, HoloTall + 0.1f, holoZ0, holoZ1);
        Solid(solid, "Holodeck wall", holoX0 - wall, holoX0, 0, HoloTall, holoZ0, holoZ1);
        Solid(solid, "Holodeck wall", holoX0, holoX1, 0, HoloTall, holoZ0 - wall, holoZ0);
        Solid(solid, "Holodeck wall", holoX0, holoX1, 0, HoloTall, holoZ1, holoZ1 + wall);
        Solid(solid, "Corridor floor", CorX0, CorX1, -0.1f, 0, corZ0, -halfD);
        Solid(solid, "Corridor ceiling", CorX0, CorX1, CorHigh, CorHigh + 0.1f, corZ0, corZ1);
        Solid(solid, "Corridor wall", CorX0, CorX1, 0, CorHigh, corZ0 - wall, corZ0);
        Solid(solid, "Corridor wall", CorX0 - wall, CorX0, 0, CorHigh, corZ0, corZ1);
        Solid(solid, "Corridor wall", CorX1, CorX1 + wall, 0, CorHigh, corZ0, corZ1);
        Solid(solid, "Corridor bench", -2.55f, -1.35f, 0, 0.45f, corZ1 - 0.5f, corZ1);

        // ---- a visitor steps out of the lift
        foreach (GameObject go in world.gameObject.scene.GetRootGameObjects())
            if (go.name == "VRCWorld") go.transform.SetPositionAndRotation(new Vector3(CorX0 + 1.1f, 0, (corZ0 + corZ1) / 2), Quaternion.Euler(0, 90, 0));

        // ---- light: the corridor's three lamps, and a little from the holodeck's ceiling
        Gone(decor, "Corridor light");
        Gone(decor, "Holodeck light");
        foreach (float x in new[] { -3.9f, -0.4f, 3.1f })
            BakedLight(decor, "Corridor light", new Vector3(x, CorHigh - 0.3f, (corZ0 + corZ1) / 2), new Color(1f, 0.84f, 0.62f), 1.3f, 5f);
        foreach (float x in new[] { -2.2f, 2.2f })
            BakedLight(decor, "Holodeck light", new Vector3((holoX0 + holoX1) / 2 + x, HoloTall - 0.5f, HoloZ), new Color(1f, 0.85f, 0.55f), 0.9f, 9f);
        Gone(world, "Light volume corridor");
        Gone(world, "Light volume holodeck");
        LightVolumeIn(world, "Light volume corridor", new Vector3((CorX0 + CorX1) / 2, CorHigh / 2, (corZ0 + corZ1) / 2), new Vector3(CorX1 - CorX0, CorHigh, CorWide));
        LightVolumeIn(world, "Light volume holodeck", new Vector3((holoX0 + holoX1) / 2, HoloTall / 2, HoloZ), new Vector3(HoloSide, HoloTall, HoloSide));
        Gone(decor, "Reflection probe corridor");
        Gone(decor, "Reflection probe holodeck");
        ProbeIn(decor, "Reflection probe corridor", new Vector3((CorX0 + CorX1) / 2, CorHigh / 2, (corZ0 + corZ1) / 2), new Vector3(CorX1 - CorX0, CorHigh, CorWide));
        ProbeIn(decor, "Reflection probe holodeck", new Vector3((holoX0 + holoX1) / 2, HoloTall / 2, HoloZ), new Vector3(HoloSide, HoloTall, HoloSide));

        // ---- the holodeck: the volume display's renderers, with the room's own faces as the window
        Gone(computer, "Volume display");
        Gone(world, "Holodeck");
        Transform holodeck = new GameObject("Holodeck").transform;
        holodeck.SetParent(world, false);
        Material sceneMat = Mat("GpuHolodeck", "ShaderEmu/GpuHolodeck");
        Material volumeMat = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/GpuVolume.mat");
        if (volumeMat != null) sceneMat.SetTexture("_State", volumeMat.GetTexture("_State"));
        sceneMat.SetVector("_ScreenSize", new Vector4(1.28f, 0.8f, 0, 0));
        sceneMat.SetVector("_RoomMin", world.TransformPoint(new Vector3(holoX0, 0, holoZ0)));
        sceneMat.SetVector("_RoomMax", world.TransformPoint(new Vector3(holoX1, HoloTall, holoZ1)));

        Transform content = new GameObject("Holodeck content").transform;
        content.SetParent(holodeck, false);
        // (a model's own objects cannot be moved out of it: copies, and the model's put away)
        Transform maskModel = models.Find("Holodeck mask");
        Transform mask = Object.Instantiate(maskModel.gameObject, content, true).transform;
        mask.name = "Holodeck mask";
        maskModel.gameObject.SetActive(false);
        GameObject seal = Object.Instantiate(mask.gameObject, content, true);
        seal.name = "Holodeck seal";
        Material maskMat = Mat("HolodeckMask", "ShaderEmu/HolodeckMask");
        if (volumeMat != null) maskMat.SetTexture("_State", volumeMat.GetTexture("_State"));
        mask.GetComponent<MeshRenderer>().sharedMaterial = maskMat;
        seal.GetComponent<MeshRenderer>().sharedMaterial = Mat("VolumeSeal", "ShaderEmu/VolumeSeal");
        foreach (MeshRenderer renderer in content.GetComponentsInChildren<MeshRenderer>())
        {
            GameObjectUtility.SetStaticEditorFlags(renderer.gameObject, 0);
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        }

        // the control: the program's camera, or its world, stands where a visitor puts this
        GameObject control = new GameObject("Holodeck control");
        control.transform.SetParent(holodeck, false);
        control.transform.localPosition = new Vector3((holoX0 + holoX1) / 2, 1.3f, HoloZ);
        control.transform.localEulerAngles = new Vector3(0, -90, 0);   // looking away from the door
        Transform shapeModel = models.Find("Holodeck control");
        Transform shape = Object.Instantiate(shapeModel.gameObject, control.transform, false).transform;
        shapeModel.gameObject.SetActive(false);
        shape.localPosition = Vector3.zero;
        shape.localRotation = Quaternion.identity;
        shape.name = "Model";
        GameObjectUtility.SetStaticEditorFlags(shape.gameObject, 0);
        control.AddComponent<BoxCollider>().size = new Vector3(0.22f, 0.14f, 0.22f);
        Rigidbody body = control.AddComponent<Rigidbody>();
        body.isKinematic = true;     // it stays where it is let go, in the air
        body.useGravity = false;
        VRC.SDK3.Components.VRCPickup pickup = control.AddComponent<VRC.SDK3.Components.VRCPickup>();
        pickup.InteractionText = "Move the program's world";
        pickup.orientation = VRC.SDKBase.VRC_Pickup.PickupOrientation.Any;
        pickup.AutoHold = VRC.SDKBase.VRC_Pickup.AutoHoldMode.No;

        Mesh points = LoadOrCreate(Generated + "/HolodeckPoints.asset", () =>
        {
            int[] indices = new int[GpuTriangles];
            for (int i = 0; i < GpuTriangles; i++) indices[i] = i;
            Mesh mesh = new Mesh { name = "HolodeckPoints", indexFormat = IndexFormat.UInt32 };
            mesh.vertices = new Vector3[GpuTriangles];
            mesh.SetIndices(indices, MeshTopology.Points, 0, false);
            return mesh;
        });
        points.bounds = new Bounds(Vector3.zero, Vector3.one * 2000f);   // the world reaches where it will
        EditorUtility.SetDirty(points);
        GameObject scene = new GameObject("Holodeck scene", typeof(MeshFilter), typeof(MeshRenderer));
        scene.transform.SetParent(control.transform, false);
        scene.GetComponent<MeshFilter>().sharedMesh = points;
        MeshRenderer sceneRenderer = scene.GetComponent<MeshRenderer>();
        sceneRenderer.sharedMaterial = sceneMat;
        sceneRenderer.shadowCastingMode = ShadowCastingMode.Off;
        sceneRenderer.receiveShadows = false;
        sceneRenderer.lightProbeUsage = LightProbeUsage.Off;
        sceneRenderer.reflectionProbeUsage = ReflectionProbeUsage.Off;

        // ---- its controls, on the plate by the door in the den
        RectTransform panel = Panel(holodeck, "Holodeck panel", new Vector3(-halfW + 0.018f, 1.45f, HoloZ - HoloWide / 2 - 0.7f), new Vector3(0, -90f, 0),
                                    760, 480, new Color(0.05f, 0.055f, 0.07f));
        EmuVolume volume = Udon<EmuVolume>(panel.gameObject);
        volume.content = content.gameObject;
        volume.scene = scene;
        volume.sceneMaterial = sceneMat;
        volume.control = control.transform;
        RenderTexture origin = LoadOrCreate(Generated + "/HolodeckOrigin.renderTexture",
            () => new RenderTexture(1, 1, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear));
        origin.filterMode = FilterMode.Point;
        volume.originTexture = origin;
        volume.originMaterial = Mat("HolodeckOrigin", "ShaderEmu/HolodeckOrigin");
        sceneMat.SetTexture("_Origin", origin);
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        Label(panel, "Title", "Holodeck", 20, 12, 400, 46, 30, TextAnchor.MiddleLeft, Color.white);
        Button power = MakeButton(panel, "Power", "Program: on", 20, 70, 230, 70, 26, out volume.powerLabel);
        TMPro.TextMeshProUGUI recallLabel;
        Button recall = MakeButton(panel, "Recall", "Recall control", 265, 70, 230, 70, 24, out recallLabel);
        Button up = MakeButton(panel, "Up", "Up: auto", 510, 70, 230, 70, 26, out volume.upLabel);
        Label(panel, "Anchor title", "At the control stands the", 20, 160, 240, 70, 20, TextAnchor.MiddleLeft, Color.white);
        Button anchor = MakeButton(panel, "Anchor", "World", 265, 160, 230, 70, 26, out volume.anchorLabel);
        TMPro.TextMeshProUGUI centreLabel;
        Button centre = MakeButton(panel, "Centre", "Centre on player", 510, 160, 230, 70, 22, out centreLabel);
        Label(panel, "Scale title", "Scale: large ... small", 20, 250, 240, 50, 20, TextAnchor.MiddleLeft, Color.white);
        GameObject sliderObject = DefaultControls.CreateSlider(new DefaultControls.Resources());
        RectTransform sliderRect = sliderObject.GetComponent<RectTransform>();
        sliderRect.SetParent(panel, false);
        sliderRect.anchorMin = sliderRect.anchorMax = new Vector2(0, 1);
        sliderRect.pivot = new Vector2(0, 1);
        sliderRect.anchoredPosition = new Vector2(265, -252);
        sliderRect.sizeDelta = new Vector2(475, 48);
        Slider slider = sliderObject.GetComponent<Slider>();
        slider.minValue = 0;
        slider.maxValue = 1;
        slider.value = 0.35f;
        foreach (Image part in sliderObject.GetComponentsInChildren<Image>())
        {
            part.material = UiMaterial();
            part.color = part.name == "Handle" ? Color.white : part.name == "Fill" ? new Color(0.3f, 0.65f, 1f) : new Color(0.2f, 0.21f, 0.25f);
        }
        slider.handleRect.sizeDelta = new Vector2(36, 0);
        DeafToWalking(slider);
        volume.planeSlider = slider;
        Label(panel, "Hint", "Start a 3D program on the computer (Quake, Doom, ClassiCube, glxgears) and walk in: its world is round you. " +
              "Carry the control that floats inside to move and turn it; Centre brings the place the player is at to it.", 20, 318, 720, 150, 20, TextAnchor.UpperLeft, dim);
        Apply(volume);
        OnClick(power, volume, "Toggle");
        OnClick(recall, volume, "Recall");
        OnClick(up, volume, "ToggleUp");
        OnClick(anchor, volume, "NextAnchor");
        OnClick(centre, volume, "Centre");
        UnityEventTools.AddStringPersistentListener(slider.onValueChanged,
            UdonSharpEditorUtility.GetBackingUdonBehaviour(volume).SendCustomEvent, "PlaneChanged");

        foreach (EmuMachine machine in world.GetComponentsInChildren<EmuMachine>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(machine);
            machine.volumeMaterial = sceneMat;
            machine.volumeMaskMaterial = maskMat;
            Apply(machine);
        }
    }

    static void LightVolumeIn(Transform world, string name, Vector3 centre, Vector3 size)
    {
        GameObject holder = new GameObject(name);
        holder.transform.SetParent(world, false);
        holder.transform.localPosition = centre;
        holder.transform.localScale = size;
        VRCLightVolumes.LightVolume volume = holder.AddComponent<VRCLightVolumes.LightVolume>();
        volume.Bake = true;
        volume.AdaptiveResolution = false;
        volume.VoxelsPerUnit = 2f;
        volume.Recalculate();
    }

    static void ProbeIn(Transform decor, string name, Vector3 centre, Vector3 size)
    {
        ReflectionProbe probe = new GameObject(name).AddComponent<ReflectionProbe>();
        probe.transform.SetParent(decor, false);
        probe.transform.localPosition = centre;
        probe.mode = ReflectionProbeMode.Baked;
        probe.size = size;
        probe.boxProjection = true;
        probe.resolution = 256;
        probe.hdr = true;
    }
}
