using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;

// The windows' panes (docs/world/world.md, "The storm"): WindowRain.shader, whose drops bend the
// window probe's picture of the city (WindowProbe in ShaderEmuWeather.cs), given it as _Sky.
// A pane lit by anything of the room's shows its drops as white paint: a lightmap (a bake of a pane
// left static), light probes, or the room's probe. All that a pane needs lived in the scene's
// overrides, and each of these undid it: Blender naming a pane "Window glass.001" in another build
// order (the importer took only "Window glass"), new model exports (a renamed pane is a new object,
// none of the old one's overrides), a bake of a static pane, Unity's own bake of the window probe.
// So it is held here, not set once: every pane's settings and the probe's picture are put right
// when a model is imported, the scene opened or saved, scripts reloaded (the shader sync, the
// image import), a bake done (Unity's or Bakery's), and at play and at a build, where a pane still
// wrong fails the build. "ShaderEmu/Check the windows' glass" says what it finds.
[InitializeOnLoad]
public static class ShaderEmuGlass
{
    public const string PaneName = "Window glass";   // and "Window glass.001" and so on: Blender's names in any order
    public const string ProbeName = "Reflection probe window";
    const string Generated = "Assets/ShaderEmu/Generated";
    const string PicturePath = Generated + "/" + ProbeName + " picture.cubemap";
    const string FlashPath = Generated + "/" + ProbeName + " lightning.cubemap";   // (the same lit by a flash of 1)
    const string GlassPath = Generated + "/Glass.mat";
    const string GlassShader = "ShaderEmu/WindowRain";

    static ShaderEmuGlass()
    {
        EditorSceneManager.sceneOpened += (scene, mode) => Hold(scene, "the scene opened");
        EditorSceneManager.sceneSaving += (scene, path) => Hold(scene, "the scene saved");
        Lightmapping.bakeCompleted += () => HoldOpen("Unity's bake");
#if BAKERY_INCLUDED
        ftRenderLightmap.OnFinishedFullRender += (sender, e) => EditorApplication.delayCall += () => HoldOpen("Bakery's bake");
#endif
        EditorApplication.delayCall += () => HoldOpen("scripts reloaded");   // (after a shader sync, an image import, an editor start)
        // and a watch: an event did not always come (a model imported from a script called none), so
        // every two seconds the panes are looked at, and held if one is wrong (two renderers: no cost)
        EditorApplication.hierarchyChanged += () => watched = null;
        EditorApplication.update += Watch;
    }

    static List<MeshRenderer> watched;
    static double nextWatch;

    static void Watch()
    {
        if (EditorApplication.timeSinceStartup < nextWatch || EditorApplication.isPlayingOrWillChangePlaymode || EditorApplication.isCompiling) return;
        nextWatch = EditorApplication.timeSinceStartup + 2.0;
        Scene scene = SceneManager.GetActiveScene();
        if (watched == null) { watched = new List<MeshRenderer>(Panes(scene)); watchedProbe = Probe(scene); }
        if (watched.Count == 0) return;
        if (watchedProbe == null)   // (a scene without the window's probe: said once a minute, not mended here)
        {
            nextWatch = EditorApplication.timeSinceStartup + 60.0;
            watched = null;
            Debug.LogError("[ShaderEmu] THE WINDOWS' GLASS HAS NO " + ProbeName.ToUpper() + ": run ShaderEmu/Give the windows' glass its rain again");
            return;
        }
        Material glass = AssetDatabase.LoadAssetAtPath<Material>(GlassPath);
        bool wrong = watchedProbe.mode != ReflectionProbeMode.Custom || watchedProbe.customBakedTexture == null
                     || AssetDatabase.GetAssetPath(watchedProbe.customBakedTexture) != PicturePath;
        Material glassNow = AssetDatabase.LoadAssetAtPath<Material>(GlassPath);
        wrong |= glassNow != null && glassNow.HasProperty("_Sky") && (glassNow.GetTexture("_Sky") == null || AssetDatabase.GetAssetPath(glassNow.GetTexture("_Sky")) != PicturePath);
        wrong |= glassNow != null && glassNow.HasProperty("_SkyFlash") && AssetDatabase.LoadAssetAtPath<Cubemap>(FlashPath) != null && glassNow.GetTexture("_SkyFlash") != AssetDatabase.LoadAssetAtPath<Cubemap>(FlashPath);
        foreach (MeshRenderer pane in watched)
        {
            if (pane == null) { watched = null; return; }
            wrong |= Wrong(pane, watchedProbe, glass) != null;
        }
        if (wrong) Hold(scene, "a change (the watch)");
    }

    static ReflectionProbe watchedProbe;

    public static bool IsPane(Renderer renderer) { return renderer != null && renderer.name.StartsWith(PaneName); }

    public static void HoldOpen(string why)
    {
        if (EditorApplication.isPlayingOrWillChangePlaymode) return;
        for (int i = 0; i < SceneManager.sceneCount; i++) Hold(SceneManager.GetSceneAt(i), why);
    }

    static IEnumerable<MeshRenderer> Panes(Scene scene)
    {
        if (!scene.IsValid() || !scene.isLoaded) yield break;
        foreach (GameObject root in scene.GetRootGameObjects())
            foreach (MeshRenderer renderer in root.GetComponentsInChildren<MeshRenderer>(true))
                if (IsPane(renderer)) yield return renderer;
    }

    static ReflectionProbe Probe(Scene scene)
    {
        if (!scene.IsValid() || !scene.isLoaded) return null;
        foreach (GameObject root in scene.GetRootGameObjects())
            foreach (ReflectionProbe probe in root.GetComponentsInChildren<ReflectionProbe>(true))
                if (probe.name == ProbeName) return probe;
        return null;
    }

    // Puts every pane and the window probe right; what it changed is said, and the scene marked changed.
    public static int Hold(Scene scene, string why)
    {
        List<MeshRenderer> panes = new List<MeshRenderer>(Panes(scene));
        if (panes.Count == 0) return 0;
        List<string> fixes = new List<string>();
        Material glass = AssetDatabase.LoadAssetAtPath<Material>(GlassPath);
        if ((glass == null || glass.shader == null || glass.shader.name != GlassShader) && !Application.isPlaying && !BuildPipeline.isBuildingPlayer)
        {
            ShaderEmuBuilder.GlassAgain();   // the material made again (its shader, Mochie's textures), the probe's picture and the anchors
            glass = AssetDatabase.LoadAssetAtPath<Material>(GlassPath);
            fixes.Add("the glass material made again");
        }
        Cubemap picture = AssetDatabase.LoadAssetAtPath<Cubemap>(PicturePath);
        ReflectionProbe probe = Probe(scene);
        if (probe != null && picture != null && (probe.mode != ReflectionProbeMode.Custom || probe.customBakedTexture != picture))
        {
            probe.mode = ReflectionProbeMode.Custom;   // (a bake of Unity's had given it the lit room)
            probe.customBakedTexture = picture;
            probe.boxProjection = false;
            EditorUtility.SetDirty(probe);
            fixes.Add("the window probe's picture");
        }
        if (glass != null && picture != null && glass.HasProperty("_Sky") && glass.GetTexture("_Sky") != picture)
        {
            glass.SetTexture("_Sky", picture);   // (what the drops bend: WindowRain.shader takes the picture itself)
            EditorUtility.SetDirty(glass);
            fixes.Add("the drops' sky");
        }
        Cubemap flashed = AssetDatabase.LoadAssetAtPath<Cubemap>(FlashPath);
        if (glass != null && flashed != null && glass.HasProperty("_SkyFlash") && glass.GetTexture("_SkyFlash") != flashed)
        {
            glass.SetTexture("_SkyFlash", flashed);   // (lightning in the drops)
            EditorUtility.SetDirty(glass);
            fixes.Add("the drops' lightning");
        }
        foreach (MeshRenderer pane in panes)
        {
            string was = Wrong(pane, probe, glass);
            if (was == null) continue;
            if (glass != null && (pane.sharedMaterial != glass || pane.sharedMaterials.Length != 1)) pane.sharedMaterials = new[] { glass };
            if (GameObjectUtility.GetStaticEditorFlags(pane.gameObject) != 0) GameObjectUtility.SetStaticEditorFlags(pane.gameObject, 0);
            pane.shadowCastingMode = ShadowCastingMode.Off;
            pane.receiveShadows = false;
            pane.lightProbeUsage = LightProbeUsage.Off;
            pane.reflectionProbeUsage = ReflectionProbeUsage.Simple;
            if (probe != null) pane.probeAnchor = probe.transform;
            pane.lightmapIndex = -1;
            pane.lightmapScaleOffset = new Vector4(1, 1, 0, 0);
            EditorUtility.SetDirty(pane);
            EditorUtility.SetDirty(pane.gameObject);
            fixes.Add(pane.name + " (" + was + ")");
        }
        int unbaked = OutOfBakery(scene);
        if (unbaked > 0) fixes.Add(unbaked + " of Bakery's lightmap entries for a pane");
        if (fixes.Count > 0)
        {
            if (!Application.isPlaying && !BuildPipeline.isBuildingPlayer) EditorSceneManager.MarkSceneDirty(scene);   // (not the copy a play or a build takes)
            Debug.LogWarning("[ShaderEmu] the windows' glass put right after " + why + ": " + string.Join(", ", fixes));
        }
        return fixes.Count;
    }

    // What is wrong with a pane, or null
    static string Wrong(MeshRenderer pane, ReflectionProbe probe, Material glass)
    {
        List<string> wrong = new List<string>();
        Material m = pane.sharedMaterial;
        if (m == null || m.shader == null || m.shader.name != GlassShader || (glass != null && m != glass)) wrong.Add("material " + (m != null ? m.name : "none"));
        if (GameObjectUtility.GetStaticEditorFlags(pane.gameObject) != 0) wrong.Add("static");
        if (pane.lightmapIndex >= 0 && pane.lightmapIndex < 65534) wrong.Add("lightmapped");
        if (pane.lightProbeUsage != LightProbeUsage.Off) wrong.Add("light probes");
        if (pane.reflectionProbeUsage != ReflectionProbeUsage.Simple) wrong.Add("probe blending");
        if (probe == null || pane.probeAnchor != probe.transform) wrong.Add("not anchored to the window probe");
        if (pane.shadowCastingMode != ShadowCastingMode.Off || pane.receiveShadows) wrong.Add("shadows");
        return wrong.Count == 0 ? null : string.Join(", ", wrong);
    }

    // Bakery's own list gives each renderer its lightmap when the scene loads, in VRChat too: a pane's entry is none
    static int OutOfBakery(Scene scene)
    {
        int count = 0;
#if BAKERY_INCLUDED
        foreach (GameObject root in scene.GetRootGameObjects())
            foreach (ftLightmapsStorage storage in root.GetComponentsInChildren<ftLightmapsStorage>(true))
            {
                if (storage.bakedRenderers == null || storage.bakedIDs == null) continue;
                for (int i = 0; i < storage.bakedRenderers.Count && i < storage.bakedIDs.Count; i++)
                {
                    if (!IsPane(storage.bakedRenderers[i]) || storage.bakedIDs[i] < 0) continue;
                    storage.bakedIDs[i] = -1;
                    count++;
                }
                if (count > 0) EditorUtility.SetDirty(storage);
            }
#endif
        return count;
    }

    // What is still wrong after holding: empty when all is well
    public static List<string> Problems(Scene scene)
    {
        List<string> problems = new List<string>();
        List<MeshRenderer> panes = new List<MeshRenderer>(Panes(scene));
        if (panes.Count == 0) return problems;   // (a scene without the room)
        Material glass = AssetDatabase.LoadAssetAtPath<Material>(GlassPath);
        ReflectionProbe probe = Probe(scene);
        if (glass == null) problems.Add(GlassPath + " is missing (ShaderEmu/Give the windows' glass its rain again)");
        if (probe == null) problems.Add("no " + ProbeName + " (ShaderEmu/Give the windows' glass its rain again)");
        else if (probe.customBakedTexture == null || AssetDatabase.GetAssetPath(probe.customBakedTexture) != PicturePath)
            problems.Add("the window probe does not show " + PicturePath + " (ShaderEmu/Give the windows' glass its rain again)");
        if (glass != null && (!glass.HasProperty("_Sky") || glass.GetTexture("_Sky") == null || AssetDatabase.GetAssetPath(glass.GetTexture("_Sky")) != PicturePath))
            problems.Add("the glass's drops have no sky (" + PicturePath + ")");
        if (glass != null && glass.HasProperty("_SkyFlash") && (glass.GetTexture("_SkyFlash") == null || AssetDatabase.GetAssetPath(glass.GetTexture("_SkyFlash")) != FlashPath))
            problems.Add("the glass's drops have no lightning (" + FlashPath + ")");
        foreach (MeshRenderer pane in panes)
        {
            string was = Wrong(pane, probe, glass);
            if (was != null) problems.Add(pane.name + ": " + was);
        }
#if BAKERY_INCLUDED
        foreach (GameObject root in scene.GetRootGameObjects())
            foreach (ftLightmapsStorage storage in root.GetComponentsInChildren<ftLightmapsStorage>(true))
                if (storage.bakedRenderers != null && storage.bakedIDs != null)
                    for (int i = 0; i < storage.bakedRenderers.Count && i < storage.bakedIDs.Count; i++)
                        if (IsPane(storage.bakedRenderers[i]) && storage.bakedIDs[i] >= 0) problems.Add(storage.bakedRenderers[i].name + " has a lightmap in Bakery's list");
#endif
        return problems;
    }

    [MenuItem("ShaderEmu/Check the windows' glass")]
    public static void Check()
    {
        Scene scene = SceneManager.GetActiveScene();
        int fixes = Hold(scene, "a check");
        List<string> problems = Problems(scene);
        if (problems.Count > 0) Debug.LogError("[ShaderEmu] THE WINDOWS' GLASS IS WRONG: " + string.Join("; ", problems));
        else Debug.Log("[ShaderEmu] the windows' glass is right (" + new List<MeshRenderer>(Panes(scene)).Count + " panes, " + fixes + " things put right)");
    }
}

// A model import: the panes of the room's models come out unlit by the room whatever the scene says,
// and the scene is held again once the models are in it.
class ShaderEmuGlassImport : AssetPostprocessor
{
    void OnPostprocessModel(GameObject model)
    {
        if (!assetPath.StartsWith("Assets/ShaderEmu/Models/")) return;
        foreach (MeshRenderer renderer in model.GetComponentsInChildren<MeshRenderer>(true))
        {
            if (!ShaderEmuGlass.IsPane(renderer)) continue;
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Simple;
            GameObjectUtility.SetStaticEditorFlags(renderer.gameObject, 0);
        }
    }

    static void OnPostprocessAllAssets(string[] imported, string[] deleted, string[] moved, string[] movedFrom)
    {
        foreach (string path in imported)
            if (path.StartsWith("Assets/ShaderEmu/Models/") || path == "Assets/ShaderEmu/Generated/Glass.mat")
            {
                EditorApplication.delayCall += () => ShaderEmuGlass.HoldOpen("a model import");
                return;
            }
    }
}

// At play and at a build: held, then checked; a pane still wrong fails the build, loudly.
class ShaderEmuGlassBuild : IProcessSceneWithReport
{
    public int callbackOrder => -10;

    public void OnProcessScene(Scene scene, BuildReport report)
    {
        ShaderEmuGlass.Hold(scene, report == null ? "play" : "a build");
        List<string> problems = ShaderEmuGlass.Problems(scene);
        if (problems.Count == 0) return;
        string text = "[ShaderEmu] THE WINDOWS' GLASS IS WRONG: " + string.Join("; ", problems);
        if (report != null) throw new BuildFailedException(text);
        Debug.LogError(text);
    }
}
