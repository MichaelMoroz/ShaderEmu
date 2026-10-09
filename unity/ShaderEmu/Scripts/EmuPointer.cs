using UdonSharp;
using UnityEngine;
using TMPro;
using VRC.SDKBase;
using VRC.Udon.Common;

// The players' beams: one per hand in VR, each drawn as a line with a dot where it lands, and
// the middle of the view on desktop. A beam on the display is the machine's pointer
// (docs/input.md): trigger or click is the left button, grip or right click the right one.
// A beam on a keyboard holds the key under it while the trigger is held; both hands type.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPointer : UdonSharpBehaviour
{
    public EmuMachine machine;
    public Collider screen;            // on the display quad (a unit quad, scaled)
    public float aspect = 1.7777778f;  // the quad's width / height, as Display.shader has it
    public float maxDistance = 12f;
    public EmuKeyboard[] keyboards;
    public Collider[] keyboardPlates;  // one a keyboard, same order
    public LineRenderer[] beams;       // left hand, right hand
    public Transform[] dots;
    public Renderer[] dotRenderers;
    public Material faintMaterial;     // a beam that only points
    public Material solidMaterial;     // a beam whose trigger or grip is held
    public float pitch;                // degrees the beam is tipped towards the palm
    public TextMeshProUGUI pitchLabel;
    [HideInInspector] public bool away;   // the game controller is in the player's hands: no beams (EmuGamepad)

    private const int Nothing = -1, Display = 0;   // a target; 1 and up: keyboard number + 1

    private int[] target = new int[2];
    private int[] key = new int[2];
    private int[] px = new int[2];
    private int[] py = new int[2];
    private bool[] trigger = new bool[2];
    private bool[] grip = new bool[2];
    private int[] pressedOn = new int[2];   // the keyboard a hand holds a key on, or -1
    private int owner = 1;                  // the hand the machine's pointer follows
    private float stick;                    // the right stick, up positive
    private float turned;                   // notches of the wheel not sent yet
    private bool started;
    private Collider linksPlate;            // the links panel, in front of the display while open

    private void Init()
    {
        if (started) return;
        started = true;
        for (int h = 0; h < 2; h++)
        {
            target[h] = Nothing;
            key[h] = -1;
            pressedOn[h] = -1;
        }
        if (machine.linksPanel != null) linksPlate = machine.linksPanel.GetComponent<Collider>();
        ShowPitch();
    }

    void Start()
    {
        Init();
    }

    // ---- where a hand points ----

    // Along the back of the hand, from the avatar's bones: wrist to knuckles, or elbow to wrist
    // for a hand without fingers. False when the avatar has neither.
    private bool BoneRay(VRCPlayerApi player, bool leftHand, out Vector3 origin, out Vector3 direction)
    {
        Vector3 wrist = player.GetBonePosition(leftHand ? HumanBodyBones.LeftHand : HumanBodyBones.RightHand);
        Vector3 middle = player.GetBonePosition(leftHand ? HumanBodyBones.LeftMiddleProximal : HumanBodyBones.RightMiddleProximal);
        Vector3 index = player.GetBonePosition(leftHand ? HumanBodyBones.LeftIndexProximal : HumanBodyBones.RightIndexProximal);
        Vector3 little = player.GetBonePosition(leftHand ? HumanBodyBones.LeftLittleProximal : HumanBodyBones.RightLittleProximal);
        origin = wrist;
        direction = Vector3.forward;
        if (wrist == Vector3.zero) return false;
        if (middle != Vector3.zero && (middle - wrist).sqrMagnitude > 1e-6f)
        {
            origin = middle;
            direction = (middle - wrist).normalized;
        }
        else
        {
            Vector3 elbow = player.GetBonePosition(leftHand ? HumanBodyBones.LeftLowerArm : HumanBodyBones.RightLowerArm);
            if (elbow == Vector3.zero || (wrist - elbow).sqrMagnitude < 1e-6f) return false;
            direction = (wrist - elbow).normalized;
        }
        if (pitch != 0f && index != Vector3.zero && little != Vector3.zero)
        {
            // the palm faces along this; turning about the line across the knuckles tips the beam
            Vector3 palm = Vector3.Cross(index - wrist, little - wrist);
            if (!leftHand) palm = -palm;
            Vector3 across = Vector3.Cross(palm, direction);
            if (across.sqrMagnitude > 1e-9f) direction = Quaternion.AngleAxis(pitch, across.normalized) * direction;
        }
        return true;
    }

    // What the ray meets first of the display and the keyboards.
    private void Aim(int h, Vector3 origin, Vector3 direction, bool drawBeam)
    {
        Ray ray = new Ray(origin, direction);
        RaycastHit hit;
        float best = maxDistance;
        int found = Nothing, foundKey = -1;
        bool landed = false;
        Vector3 at = origin;
        // a beam that meets the open links panel is VRChat's to use, not the machine's pointer
        bool covered = linksPlate != null && machine.linksPanel.activeSelf && linksPlate.Raycast(ray, out hit, best);
        if (!covered && screen.Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            int w = machine.shownWidth, ht = machine.shownHeight;   // another player's, while theirs is shown
            if (w > 0 && ht > 0)
            {
                Vector3 local = screen.transform.InverseTransformPoint(hit.point);
                // the picture sits centred in the quad, as large as fits
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, aspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / aspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                }
            }
        }
        for (int k = 0; k < keyboardPlates.Length; k++)
        {
            if (!keyboardPlates[k].Raycast(ray, out hit, best)) continue;
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = k + 1;
            foundKey = keyboards[k].KeyAt(hit.point);
        }

        // What stands between stops the beam (a classroom's monitor before the wall's display): it
        // used to reach through, and move the pointer from behind a thing the hand pointed at.
        if (landed && Physics.Raycast(ray, out hit, best - 0.03f, 1, QueryTriggerInteraction.Ignore))
        {
            landed = false;
            found = Nothing;
            foundKey = -1;
        }

        // the key under the beam lights up; the one it left goes dark
        int was = target[h];
        if (was >= 1 && was != found) keyboards[was - 1].SetHover(h, -1);
        if (found >= 1) keyboards[found - 1].SetHover(h, foundKey);
        target[h] = found;
        key[h] = foundKey;

        // nothing is drawn for a hand that points at neither
        beams[h].enabled = drawBeam && landed;
        if (drawBeam && landed)
        {
            beams[h].SetPosition(0, origin);
            beams[h].SetPosition(1, at);
        }
        dots[h].gameObject.SetActive(landed);
        if (landed)
        {
            dots[h].position = at;
            Material material = trigger[h] || grip[h] || pressedOn[h] >= 0 ? solidMaterial : faintMaterial;
            beams[h].sharedMaterial = material;
            dotRenderers[h].sharedMaterial = material;
        }
    }

    private void Idle(int h)
    {
        if (target[h] >= 1) keyboards[target[h] - 1].SetHover(h, -1);
        target[h] = Nothing;
        key[h] = -1;
        beams[h].enabled = false;
        dots[h].gameObject.SetActive(false);
    }

    // After the avatar has been posed for this frame, so the beams sit on the hands.
    public override void PostLateUpdate()
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        if (away)
        {
            Idle(0);
            Idle(1);
            return;
        }
        if (player.IsUserInVR())
        {
            for (int h = 0; h < 2; h++)
            {
                Vector3 origin, direction;
                if (!BoneRay(player, h == 0, out origin, out direction))
                {
                    // no arm bones: the tracked hand, whose fingers lie along its x axis
                    VRCPlayerApi.TrackingData hand = player.GetTrackingData(
                        h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
                    origin = hand.position;
                    direction = hand.rotation * (h == 0 ? Vector3.left : Vector3.right);
                }
                Aim(h, origin, direction, true);
            }
        }
        else
        {
            Idle(0);
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            Aim(1, head.position, head.rotation * Vector3.forward, false);
            grip[1] = target[1] == Display && Input.GetMouseButton(1);
        }

        // The machine has one pointer: a hand holding a button on the display keeps it, then
        // the hand that had it, then whichever points there.
        int other = 1 - owner;
        bool ownerHolds = target[owner] == Display && (trigger[owner] || grip[owner]);
        bool otherHolds = target[other] == Display && (trigger[other] || grip[other]);
        if (!ownerHolds && (otherHolds || (target[owner] != Display && target[other] == Display))) owner = other;
        if (target[owner] == Display)
        {
            machine.SetPointer(px[owner], py[owner], (trigger[owner] ? 1 : 0) | (grip[owner] ? 2 : 0));
            // the wheel: the mouse's on desktop, the right stick in VR at up to 12 notches a second
            if (player.IsUserInVR()) turned += stick * 12f * Time.deltaTime;
            else turned += Input.GetAxis("Mouse ScrollWheel") * 10f;
            int notches = (int)turned;
            if (notches != 0)
            {
                turned -= notches;
                machine.Wheel(notches);
            }
        }
        else
        {
            machine.SetPointerButtons(0);
            turned = 0f;
        }
    }

    public override void InputLookVertical(float value, UdonInputEventArgs args)
    {
        stick = Mathf.Abs(value) < 0.25f ? 0f : value;
    }

    // ---- triggers and grips ----

    private int Hand(UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return 1;
        return args.handType == HandType.LEFT ? 0 : 1;
    }

    public override void InputUse(bool value, UdonInputEventArgs args)
    {
        Init();
        if (away && value) return;
        int h = Hand(args);
        if (value)
        {
            if (target[h] == Display)
            {
                trigger[h] = true;
                owner = h;
            }
            else if (target[h] >= 1 && pressedOn[h] < 0)
            {
                pressedOn[h] = target[h] - 1;
                keyboards[pressedOn[h]].Press(h, key[h]);
            }
        }
        else
        {
            trigger[h] = false;
            if (pressedOn[h] >= 0) keyboards[pressedOn[h]].Release(h);
            pressedOn[h] = -1;
        }
    }

    public override void InputGrab(bool value, UdonInputEventArgs args)
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return;
        int h = Hand(args);
        grip[h] = value && target[h] == Display && !away;
    }

    // ---- the beam's angle (buttons on the control panel) ----

    public void PitchUp()
    {
        pitch = Mathf.Clamp(pitch - 5f, -60f, 60f);
        ShowPitch();
    }

    public void PitchDown()
    {
        pitch = Mathf.Clamp(pitch + 5f, -60f, 60f);
        ShowPitch();
    }

    private void ShowPitch()
    {
        if (pitchLabel != null) pitchLabel.text = "Beam angle " + (-pitch).ToString("F0");
    }
}
