using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// A holodeck's seat (docs/holodeck.md): a station of the scene, which VRChat syncs by itself,
// one visitor at a time. It only says who sat down and who got up.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuSeat : UdonSharpBehaviour
{
    public EmuHolodeck holodeck;
    public int room;

    public override void Interact()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null) player.UseAttachedStation();
    }

    public override void OnStationEntered(VRCPlayerApi player)
    {
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Entered(room, player);
    }

    public override void OnStationExited(VRCPlayerApi player)
    {
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Exited(room, player);
    }
}
