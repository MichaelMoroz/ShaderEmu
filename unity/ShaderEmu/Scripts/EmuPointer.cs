using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// The machine's pointer (docs/input.md): where the player points on the display. In VR a ray
// from a hand, on desktop from the middle of the view. Trigger or left click is the left
// button; grip or right click the right one.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPointer : UdonSharpBehaviour
{
    public EmuMachine machine;
    public Collider screen;            // on the display quad (a unit quad, scaled)
    public float aspect = 1.7777778f;  // the quad's width / height, as Display.shader has it
    public float maxDistance = 12f;
    public Vector3 vrRayEuler;         // turns the hand's forward into where its laser points
    public bool longPressRightClick;   // holding the left button still becomes a right click
    public float longPressSeconds = 0.6f;

    private bool aiming;
    private bool aimingLeftHand;
    private int px, py;
    private bool left, right;
    private float downAt;
    private int downX, downY;
    private bool converted;

    private bool Aim(Vector3 origin, Quaternion rotation)
    {
        RaycastHit hit;
        if (!Physics.Raycast(origin, rotation * Vector3.forward, out hit, maxDistance)) return false;
        if (hit.collider != screen) return false;
        int w = machine.displayWidth, h = machine.displayHeight;
        if (w <= 0 || h <= 0) return false;
        Vector3 local = screen.transform.InverseTransformPoint(hit.point);
        // the picture sits centred in the quad, as large as fits
        float shape = (float)w / h;
        float x = local.x * Mathf.Max(1f, aspect / shape) + 0.5f;
        float y = -local.y * Mathf.Max(1f, shape / aspect) + 0.5f;
        if (x < 0f || y < 0f || x >= 1f || y >= 1f) return false;
        px = (int)(x * w);
        py = (int)(y * h);
        return true;
    }

    void Update()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        bool hit = false;
        if (player.IsUserInVR())
        {
            Quaternion turn = Quaternion.Euler(vrRayEuler);
            // a hand holding a button keeps the pointer; otherwise right before left
            for (int k = 0; k < 2 && !hit; k++)
            {
                bool leftHand = (k == 1) != ((left || right) && aimingLeftHand);
                VRCPlayerApi.TrackingData hand = player.GetTrackingData(
                    leftHand ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
                if (Aim(hand.position, hand.rotation * turn))
                {
                    hit = true;
                    aimingLeftHand = leftHand;
                }
            }
        }
        else
        {
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            hit = Aim(head.position, head.rotation);
            if (!converted) right = hit && Input.GetMouseButton(1);
        }
        aiming = hit;
        if (!hit)
        {
            left = false;
            right = false;
        }
        else if (longPressRightClick && left && !converted && Time.time - downAt >= longPressSeconds
                 && Mathf.Abs(px - downX) < 8 && Mathf.Abs(py - downY) < 8)
        {
            left = false;
            right = true;
            converted = true;
        }
        if (hit) machine.SetPointer(px, py, (left ? 1 : 0) | (right ? 2 : 0));
        else machine.SetPointerButtons(0);
    }

    private bool HandMatches(UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return true;
        return (args.handType == HandType.LEFT) == aimingLeftHand;
    }

    public override void InputUse(bool value, UdonInputEventArgs args)
    {
        if (!HandMatches(args)) return;
        if (value && aiming)
        {
            left = true;
            converted = false;
            downAt = Time.time;
            downX = px;
            downY = py;
        }
        else if (!value)
        {
            left = false;
            if (converted) right = false;
            converted = false;
        }
    }

    public override void InputGrab(bool value, UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR() || !HandMatches(args)) return;
        right = value && aiming;
    }
}
