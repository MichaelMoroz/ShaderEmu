using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// Voices between the holodecks (docs/holodeck.md): whoever is in a holodeck hears everybody who
// is in one, the same or another, at full strength whatever the distance. Between inside and
// outside a voice travels as VRChat's does. Each client decides for itself, from where the
// players are; nothing is synced.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuVoices : UdonSharpBehaviour
{
    public Vector3[] low, high;        // each room's box, in the world
    public float far = 400f;           // how far a voice carries between the rooms
    public float nearUsual = 0f, farUsual = 25f;   // VRChat's own
    public bool log;                   // a line in the output log whenever the number so heard changes

    private VRCPlayerApi[] players = new VRCPlayerApi[100];
    private float nextAt;
    private int linked = -1;

    private bool Inside(Vector3 at)
    {
        for (int k = 0; k < low.Length; k++)
            if (at.x >= low[k].x && at.x <= high[k].x && at.y >= low[k].y - 0.5f && at.y <= high[k].y && at.z >= low[k].z && at.z <= high[k].z) return true;
        return false;
    }

    void Update()
    {
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.5f;
        VRCPlayerApi me = Networking.LocalPlayer;
        if (!Utilities.IsValid(me)) return;
        bool here = Inside(me.GetPosition());
        int n = Mathf.Min(VRCPlayerApi.GetPlayerCount(), players.Length), now = 0;
        VRCPlayerApi.GetPlayers(players);
        for (int i = 0; i < n; i++)
        {
            VRCPlayerApi player = players[i];
            if (!Utilities.IsValid(player) || player.isLocal) continue;
            bool both = here && Inside(player.GetPosition());
            // near as far as far: no falling off with distance
            player.SetVoiceDistanceNear(both ? far : nearUsual);
            player.SetVoiceDistanceFar(both ? far : farUsual);
            if (both) now++;
        }
        if (now != linked)
        {
            linked = now;
            if (log) Debug.Log("[Voices] t=" + Time.time.ToString("F0") + " in a holodeck=" + here + " heard at full strength=" + now + " of " + (n - 1));
        }
    }
}
