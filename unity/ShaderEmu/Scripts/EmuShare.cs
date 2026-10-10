using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// What one player sends the others about their machine (docs/share.md). A player object:
// VRChat makes one a player, owned by that player. EmuShareHub fills the owner's and reads
// everyone else's; this only carries.
[UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
public class EmuShare : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public Transform[] pads;             // the two controllers' models, for this player's hand (EmuHolodeck)

    [UdonSynced] public int flags;       // bit 0: the machine is on; bit 1: watchers may use it
    [UdonSynced] public int watching;    // the player whose machine this one looks at on the wall; 0: their own
    [UdonSynced] public int seq;         // of the packet
    [UdonSynced] public byte[] packet = new byte[0];   // console rows and display tiles
    [UdonSynced] public int inputSeq;    // events typed at the watched machine so far
    [UdonSynced] public int[] input = new int[16];     // the last 16 of them
    [UdonSynced] public int pointer;     // x, y (12 bits each), buttons << 24, on the display << 28
    [UdonSynced] public int station = -1;   // the classroom's computer this player has (docs/stations.md); -1: none
    [UdonSynced] public int stationMode; // what that computer's tube shows: 0 the display, 1 the console
    // The senders this player shows on some screen (EmuStreams), one a slot: the sender's number
    // (16 bits) and a count << 16 that goes up with each asking; and what is asked for again:
    // a packet number's low 16 bits and how many from it << 16 (none: everything).
    [UdonSynced] public int[] ask = new int[12];
    [UdonSynced] public int[] lost = new int[12];
    [UdonSynced] public int holo = -1;   // the holodeck whose seat this player sits in (docs/holodeck.md); -1: none
    [UdonSynced] public int holding;     // the game controller in their hand: 0 none, 1 shooter, 2 strategy
    // The network (docs/lan.md): this player's machine's packets since the last sending, each
    // after two bytes of its length; netSeq goes up with every such batch.
    [UdonSynced] public int netSeq;
    [UdonSynced] public byte[] net = new byte[0];

    // what this client's hub knows of the player
    [HideInInspector] public int ownerId;
    [HideInInspector] public string ownerName = "";
    [HideInInspector] public int seenInput;
    [HideInInspector] public int seenNet;
    [HideInInspector] public int[] seenAsk = new int[12];
    [HideInInspector] public bool watchedMe;

    private bool sending;
    private float sendingSince;

    // A serialization asked for has not gone out yet.
    public bool Busy()
    {
        return sending && Time.time - sendingSince < 1f;
    }

    public void Send()
    {
        sending = true;
        sendingSince = Time.time;
        RequestSerialization();
    }

    public override void OnPostSerialization(SerializationResult result)
    {
        sending = false;
        if (!result.success && hub != null) hub.SendFailed();
    }

    public override void OnDeserialization()
    {
        if (hub != null) hub.Received(this);
    }
}
