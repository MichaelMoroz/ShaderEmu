using UnityEditor;
using UnityEngine;
using VRC.SDK3.Components;

// Things a visitor may pick up, throw and put down (docs/world/world.md, "Things to pick up"): some of what lies
// about, Poly Haven's models that are each a renderer of their own (world/fetch.py's MODELS; a model the world's
// scripts make is joined into its part's mesh and is not one). Each gets a body (Rigidbody, a convex collider),
// VRChat's pickup and its object sync, so everybody sees it where its holder or its fall left it; a thing that falls
// out of the world goes back where it lay. Not static: lit by the light volume and the probes, not the lightmaps.
public static partial class ShaderEmuBuilder
{
    // a model's name (as its renderer's starts) and its mass in kg: a real one's, from its maker or a measure
    // (a regulation baseball 142 to 149 g, a size 5 football 410 to 450, a wooden bat about 900, a C60 tape 45 in
    // its shell; the duck, the elephant, the cameras and the player weighed as their kinds weigh, a guess each)
    static readonly (string model, float kg)[] Grabbable =
    {
        ("rubber_duck_toy", 0.08f), ("football", 0.43f), ("baseball_01", 0.145f), ("baseball_bat", 0.9f),
        ("carved_wooden_elephant", 0.25f), ("Camera_01", 0.45f), ("vintage_video_camera", 1.4f),
        ("portable_cassette_player", 0.35f),   // (its tape, a renderer of the same name, is told apart by its size below)
    };
    const float TapeLargest = 0.12f, TapeKg = 0.045f;   // a compact cassette is 100.4 x 63.8 x 12 mm

    static void Grabbables(Transform world)
    {
        Transform models = world.Find("Models");
        if (models == null) { Debug.LogError("[ShaderEmu] no Models to make things to pick up of"); return; }
        int made = 0;
        foreach (MeshRenderer renderer in models.GetComponentsInChildren<MeshRenderer>(true))
        {
            float kg = -1f;
            foreach (var (model, mass) in Grabbable)
                if (renderer.name == model || renderer.name.StartsWith(model + " ")) { kg = mass; break; }
            if (kg < 0f) continue;
            MeshFilter filter = renderer.GetComponent<MeshFilter>();
            if (filter == null || filter.sharedMesh == null) continue;
            GameObject thing = renderer.gameObject;
            if (renderer.name.StartsWith("portable_cassette_player") && Vector3.Scale(filter.sharedMesh.bounds.size, thing.transform.lossyScale).magnitude < TapeLargest * 1.3f) kg = TapeKg;
            GameObjectUtility.SetStaticEditorFlags(thing, 0);
            renderer.receiveGI = ReceiveGI.LightProbes;
            renderer.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.BlendProbes;
            foreach (Collider old in thing.GetComponents<Collider>()) Object.DestroyImmediate(old);
            MeshCollider shape = thing.AddComponent<MeshCollider>();
            shape.sharedMesh = filter.sharedMesh;
            shape.convex = true;
            Rigidbody body = thing.GetComponent<Rigidbody>();
            if (body == null) body = thing.AddComponent<Rigidbody>();
            body.mass = kg;
            body.interpolation = RigidbodyInterpolation.Interpolate;
            body.collisionDetectionMode = CollisionDetectionMode.ContinuousDynamic;   // (a thrown tape is thin)
            VRCPickup pickup = thing.GetComponent<VRCPickup>();
            if (pickup == null) pickup = thing.AddComponent<VRCPickup>();
            pickup.AutoHold = VRC.SDKBase.VRC_Pickup.AutoHoldMode.Yes;
            pickup.InteractionText = "Pick up";
            if (thing.GetComponent<VRCObjectSync>() == null) thing.AddComponent<VRCObjectSync>();
            thing.layer = LayerMask.NameToLayer("Pickup");
            EditorUtility.SetDirty(thing);
            made++;
        }
        Debug.Log("[ShaderEmu] " + made + " things to pick up");
    }
}
