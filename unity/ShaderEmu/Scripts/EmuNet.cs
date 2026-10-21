using UdonSharp;
using UnityEngine;
using VRC.SDK3.UdonNetworkCalling;
using VRC.SDKBase;
using VRC.Udon.Common;

// One player's machine's network packets for the others (docs/system/lan.md): a player object of
// its own, never behind a sending of the display. A batch goes as a network event (Packets), or
// as a serialization of the fields; EmuShareHub fills the owner's and reads everyone else's.
[UdonBehaviourSyncMode(BehaviourSyncMode.Manual)]
public class EmuNet : UdonSharpBehaviour
{
    public EmuShareHub hub;

    [UdonSynced] public int seq;          // goes up with every batch
    [UdonSynced] public int sentMs;       // the instance's clock when the batch was handed to VRChat
    [UdonSynced] public byte[] packets = new byte[0];   // each after two bytes of its length

    // what this client's hub knows of the player
    [HideInInspector] public EmuShare share;
    [HideInInspector] public int seen;

    private bool sending;
    private float sendingSince;
    [HideInInspector] public float lastPostSeconds;   // from the asking to VRChat's sending, the last batch

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
        lastPostSeconds = Time.time - sendingSince;
    }

    public override void OnDeserialization()
    {
        if (hub != null) hub.ReceivedNet(this);
    }

    // A batch as a network event (EmuShareHub.netEvents): not held for the next serialization.
    [NetworkCallable(100)]
    public void Packets(byte[] batch, int number, int ms)
    {
        packets = batch;
        seq = number;
        sentMs = ms;
        if (hub != null) hub.ReceivedNet(this);
    }
}
