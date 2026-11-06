using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// The world's parts (scripts, stations, screens) are off in a build (ShaderEmuGate.cs) until the
// visitor leaves the corridor they arrive in, into the den or into one of the side rooms: a join then has nothing of ours to set up while
// VRChat is still bringing in the others' avatars and state.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuGate : UdonSharpBehaviour
{
    public GameObject[] parts;
    // the corridor by the lift alone: its south wall's face is z -8.1, and through its three doorways
    // are the side rooms (world/rooms.py), which are out of it as the den is
    public Vector3 corridorMin = new Vector3(-6.2f, -1f, -8.12f), corridorMax = new Vector3(4.6f, 4f, -5.6f);
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

    private int timed = -1;   // frames timed after the parts came on (the log says how long the gate's frame was)
    private float timedAt;

    void Update()
    {
        if (timed >= 0)
        {
            float now = Time.realtimeSinceStartup;
            Debug.Log("[Gate] frame " + timed + " after: " + ((now - timedAt) * 1000f).ToString("F0") + " ms");
            timedAt = now;
            if (++timed >= 4) enabled = false;
            return;
        }
        VRCPlayerApi me = Networking.LocalPlayer;
        if (!Utilities.IsValid(me)) return;
        Vector3 at = me.GetPosition();
        bool inside = at.x > corridorMin.x && at.x < corridorMax.x && at.y > corridorMin.y && at.y < corridorMax.y
                      && at.z > corridorMin.z && at.z < corridorMax.z;
        if (inside && !arrived) arrivedAt = Time.time;
        if (inside) arrived = true;
        if (!arrived || (inside && !(testOpen && Time.time - arrivedAt > 5f))) return;
        Debug.Log("[Gate] out of the corridor at " + at + ": " + parts.Length + " parts on");
        timedAt = Time.realtimeSinceStartup;
        foreach (GameObject part in parts) if (part != null) part.SetActive(true);
        Debug.Log("[Gate] the parts' SetActive: " + ((Time.realtimeSinceStartup - timedAt) * 1000f).ToString("F0") + " ms");
        timed = 0;
    }
}
