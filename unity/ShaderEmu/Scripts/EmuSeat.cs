using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// A holodeck's seat (docs/world/holodeck.md): a station of the scene, which VRChat syncs by itself,
// one visitor at a time. It says who sat down and who got up, and it rises under its sitter:
// VRChat puts them on a station standing and their pose sinks into sitting over half a second,
// so a seat at its height had them stand on it first. It waits on the floor and comes up as they sink.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuSeat : UdonSharpBehaviour
{
    public EmuHolodeck holodeck;
    public int room;
    public float height = 0.23f;   // where a sitter's hips are, above the floor the seat waits on
    public float settle = 0.5f;    // seconds VRChat takes from standing to sitting

    private bool taken;
    private float since;           // when the sitter came, or left
    // The box VRChat's hands and laser use the seat by is round its sitter's hands: a trigger
    // pulled there sat them down again as the console's Exit stood them up. Off while this
    // visitor sits and for a second after.
    private Collider reach;
    private bool mine;
    private int sitter = -1;       // who sits, by player number

    void Start()
    {
        reach = GetComponent<Collider>();
    }

    public override void Interact()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null) player.UseAttachedStation();
    }

    public override void OnStationEntered(VRCPlayerApi player)
    {
        taken = true;
        since = Time.time;
        sitter = Utilities.IsValid(player) ? player.playerId : -1;
        if (Utilities.IsValid(player) && player.isLocal)
        {
            mine = true;
            if (reach != null) reach.enabled = false;
        }
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Entered(room, player);
    }

    public override void OnStationExited(VRCPlayerApi player)
    {
        taken = false;
        since = Time.time;
        sitter = -1;
        if (Utilities.IsValid(player) && player.isLocal) mine = false;
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Exited(room, player);
    }

    // A sitter who leaves the instance seated is not always told as getting up: the seat stood
    // up under nobody for everyone else until somebody sat in it again.
    public override void OnPlayerLeft(VRCPlayerApi player)
    {
        if (!taken || !Utilities.IsValid(player) || player.playerId != sitter) return;
        taken = false;
        since = Time.time;
        sitter = -1;
    }

    void Update()
    {
        if (!mine && reach != null && !reach.enabled && Time.time - since > 1f) reach.enabled = true;
        Vector3 at = transform.localPosition;
        float to = at.y;
        if (taken)
        {
            // (from wherever it is: a sitter put out and sat down again at once finds it up still)
            float t = settle > 0f ? Mathf.Clamp01((Time.time - since) / settle) : 1f;
            to = Mathf.Max(at.y, height * t * t * (3f - 2f * t));
        }
        else if (Time.time - since > 0.3f) to = 0f;
        if (to == at.y) return;
        at.y = to;
        transform.localPosition = at;
    }
}
