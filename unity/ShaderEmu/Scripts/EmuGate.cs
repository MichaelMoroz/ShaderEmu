using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// The world's parts (scripts, stations, screens) are off in a build (ShaderEmuGate.cs) until the
// visitor leaves the corridor they arrive in: a join then has nothing of ours to set up while
// VRChat is still bringing in the others' avatars and state.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuGate : UdonSharpBehaviour
{
    public GameObject[] parts;
    public Vector3 corridorMin = new Vector3(-6.2f, -1f, -8.3f), corridorMax = new Vector3(4.6f, 4f, -5.6f);
    public bool testOpen;   // a test client, which never walks: the parts come on 5 s after it arrives
    private bool arrived;   // seen in the corridor: before the spawn the position is the origin
    private float arrivedAt;
    // What the parts, still off, could not hear: the local avatar's last change and the restore.
    [HideInInspector] public bool avatarSeen, restored;
    [HideInInspector] public float avatarAt;

    public override void OnAvatarChanged(VRCPlayerApi player)
    {
        if (!Utilities.IsValid(player) || !player.isLocal) return;
        avatarSeen = true;
        avatarAt = Time.time;
    }

    public override void OnPlayerRestored(VRCPlayerApi player)
    {
        if (Utilities.IsValid(player) && player.isLocal) restored = true;
    }

    void Update()
    {
        VRCPlayerApi me = Networking.LocalPlayer;
        if (!Utilities.IsValid(me)) return;
        Vector3 at = me.GetPosition();
        bool inside = at.x > corridorMin.x && at.x < corridorMax.x && at.y > corridorMin.y && at.y < corridorMax.y
                      && at.z > corridorMin.z && at.z < corridorMax.z;
        if (inside && !arrived) arrivedAt = Time.time;
        if (inside) arrived = true;
        if (!arrived || (inside && !(testOpen && Time.time - arrivedAt > 5f))) return;
        Debug.Log("[Gate] out of the corridor at " + at + ": " + parts.Length + " parts on");
        foreach (GameObject part in parts) if (part != null) part.SetActive(true);
        enabled = false;
    }
}
