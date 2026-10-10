using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// The game controllers (docs/gamepad.md): one behaviour a client. A visitor takes one only while
// they sit in a holodeck's seat; then their sticks, triggers and grips are the guest's keys and
// pointer, and the seat keeps them until the controller is put back. Which one they hold is in
// their own EmuShare, so everybody sees it in their hand (EmuHolodeck places the model).
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuGamepad : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuKeyboard keys;           // the display's keyboard: key events for the guest
    public EmuPointer pointer;         // the beams, which rest while a controller is held
    public EmuShareHub hub;
    public Transform notice;           // over the hand, the holder's own: what it is and how to put it back
    public TextMeshProUGUI noticeLabel;
    public Transform noticeBar;        // fills while both grips are held
    public float pointerSpeed = 700f;  // strategy: display pixels a second at full stick

    // Linux key codes (docs/input.md)
    private const int Esc = 1, Enter = 28, Ctrl = 29, A = 30, Z = 44, Comma = 51, Dot = 52, Slash = 53, Space = 57,
                      Up = 103, Left = 105, Right = 106, Down = 108;
    private const int Pressed = 65536;
    // what a mapping can hold down: indices into `down`
    private const int Forward = 0, Back = 1, StrafeLeft = 2, StrafeRight = 3, TurnLeft = 4, TurnRight = 5, LookUp = 6,
                      LookDown = 7, Fire = 8, Jump = 9, Next = 10, Menu = 11, Accept = 12, Held = 13;

    private int held;                       // 0 none, 1 the shooter's, 2 the strategy one
    private VRCStation seat;                // the seat it was taken in
    private float moveX, moveY, lookX, lookY;
    private bool[] trigger = new bool[2];   // left, right
    private bool[] grip = new bool[2];
    private bool jump;
    private bool[] down = new bool[Held];
    private int[] code;
    private float px, py;                   // strategy: where the pointer is
    private float turned;                   // strategy: notches of the wheel not sent yet
    private float phase;                    // shooter: where in a pulse a slight turn is
    private float bothGrips;                // seconds both grips have been held
    private float takenAt;

    void Start()
    {
        code = new int[Held];
        code[Forward] = Up;
        code[Back] = Down;
        code[StrafeLeft] = Comma;
        code[StrafeRight] = Dot;
        code[TurnLeft] = Left;
        code[TurnRight] = Right;
        code[LookUp] = A;
        code[LookDown] = Z;
        code[Fire] = Ctrl;
        code[Jump] = Space;
        code[Next] = Slash;
        code[Menu] = Esc;
        code[Accept] = Enter;
        if (notice != null) notice.gameObject.SetActive(false);
    }

    // ---- taking one and putting it back ----

    // which: 1 the shooter's, 2 the strategy one; in the seat the visitor sits in.
    public void Take(int which, VRCStation inSeat)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || inSeat == null || which < 1 || which > 2) return;
        if (held != 0) ReleaseAll();
        held = which;
        seat = inSeat;
        // the left stick is the game's now: it must not stand the sitter up
        seat.disableStationExit = true;
        pointer.away = true;
        px = machine.OwnWidth() * 0.5f;
        py = machine.OwnHeight() * 0.5f;
        bothGrips = -1f;   // (grips held now must let go before they can put it back)
        takenAt = Time.time;
        Tell(which);
    }

    public void PutBack()
    {
        if (held == 0) return;
        held = 0;
        ReleaseAll();
        machine.SetPointerButtons(0);
        pointer.away = false;
        if (seat != null) seat.disableStationExit = false;
        seat = null;
        bothGrips = -1f;
        if (notice != null) notice.gameObject.SetActive(false);
        Tell(0);
    }

    private void Tell(int which)
    {
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        mine.holding = which;
        hub.StateOut();
    }

    public override void OnPlayerRespawn(VRCPlayerApi player)
    {
        if (player != null && player.isLocal) PutBack();
    }

    public int Holding() { return held; }

    // ---- the guest's keys ----

    private void Set(int which, bool on)
    {
        if (down[which] == on) return;
        down[which] = on;
        // (this visitor's own machine's, whatever the den's wall shows: as the pointer is)
        if (machine.inputAway) machine.RemoteKey(on ? code[which] | Pressed : code[which]);
        else keys.PushEvent(on ? code[which] | Pressed : code[which]);
    }

    private void ReleaseAll()
    {
        if (code == null) return;
        for (int i = 0; i < Held; i++) Set(i, false);
    }

    // A stick's axis as a key: held when pushed far, and when pushed a little, held for that
    // share of every tenth of a second, which is a slow turn.
    private bool Pulsed(float amount)
    {
        if (amount < 0.2f) return false;
        if (amount > 0.75f) return true;
        return phase < (amount - 0.1f) / 0.65f;
    }

    void Update()
    {
        if (held == 0) return;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        // both grips together, for a second: back onto the stand (not until both were let go once)
        if (grip[0] && grip[1])
        {
            if (bothGrips >= 0f) bothGrips += Time.deltaTime;
        }
        else if (!grip[0] && !grip[1]) bothGrips = 0f;
        if (bothGrips > 1f)
        {
            PutBack();
            return;
        }
        Notice(player);

        phase = (phase + Time.deltaTime * 10f) % 1f;
        if (held == 1)
        {
            Set(Forward, moveY > 0.5f);
            Set(Back, moveY < -0.5f);
            Set(StrafeLeft, moveX < -0.5f);
            Set(StrafeRight, moveX > 0.5f);
            Set(TurnLeft, Pulsed(-lookX));
            Set(TurnRight, Pulsed(lookX));
            Set(LookUp, Pulsed(lookY));
            Set(LookDown, Pulsed(-lookY));
            Set(Fire, trigger[1]);
            Set(Jump, trigger[0]);
            Set(Next, grip[1] && !grip[0]);
            Set(Menu, grip[0] && !grip[1]);
            Set(Accept, jump);
        }
        else
        {
            int w = machine.OwnWidth(), h = machine.OwnHeight();
            bool wheel = grip[1] && !grip[0];
            if (w > 0 && h > 0)
            {
                if (!wheel)
                {
                    // faster the further the stick is pushed: fine near the middle
                    float fx = Mathf.Abs(lookX) < 0.15f ? 0f : lookX * Mathf.Abs(lookX);
                    float fy = Mathf.Abs(lookY) < 0.15f ? 0f : lookY * Mathf.Abs(lookY);
                    px = Mathf.Clamp(px + fx * pointerSpeed * Time.deltaTime, 0f, w - 1);
                    py = Mathf.Clamp(py - fy * pointerSpeed * Time.deltaTime, 0f, h - 1);
                }
                // (this visitor's own machine's, whatever the den's wall shows)
                machine.SetPointer((int)px, (int)py, (trigger[1] ? 1 : 0) | (trigger[0] ? 2 : 0));
                machine.pointerOwn = true;
            }
            turned = wheel && Mathf.Abs(lookY) > 0.25f ? turned + lookY * 12f * Time.deltaTime : 0f;
            int notches = (int)turned;
            if (notches != 0)
            {
                turned -= notches;
                machine.Wheel(notches);
            }
            Set(Forward, moveY > 0.5f);     // the arrow keys: the games scroll their maps by them
            Set(Back, moveY < -0.5f);
            Set(TurnLeft, moveX < -0.5f);
            Set(TurnRight, moveX > 0.5f);
            Set(Menu, grip[0] && !grip[1]);
            Set(Accept, jump);
        }
    }

    // Over the hand that holds it, turned to the holder's eyes: for ten seconds after taking
    // it, and whenever a grip is held, with a bar that fills until it is put back.
    private void Notice(VRCPlayerApi player)
    {
        if (notice == null) return;
        bool show = Time.time - takenAt < 10f || grip[0] || grip[1];
        if (notice.gameObject.activeSelf != show) notice.gameObject.SetActive(show);
        if (!show) return;
        bool vr = player.IsUserInVR();
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        Vector3 at = vr ? player.GetTrackingData(VRCPlayerApi.TrackingDataType.RightHand).position + Vector3.up * 0.16f
                        : head.position + head.rotation * new Vector3(0f, -0.12f, 0.5f);
        notice.SetPositionAndRotation(at, Quaternion.LookRotation(at - head.position, Vector3.up));
        if (noticeLabel != null)
        {
            string says = (held == 1 ? "SHOOTER" : "STRATEGY") + " controller\n" + (vr ? "hold BOTH GRIPS to put it back" : "G puts it back");
            if (noticeLabel.text != says) noticeLabel.text = says;
        }
        if (noticeBar != null) noticeBar.localScale = new Vector3(Mathf.Clamp01(bothGrips), 1f, 1f);
    }

    // ---- the player's sticks and buttons ----

    public override void InputMoveHorizontal(float value, UdonInputEventArgs args) { moveX = value; }
    public override void InputMoveVertical(float value, UdonInputEventArgs args) { moveY = value; }
    public override void InputLookHorizontal(float value, UdonInputEventArgs args) { lookX = value; }
    public override void InputLookVertical(float value, UdonInputEventArgs args) { lookY = value; }
    public override void InputJump(bool value, UdonInputEventArgs args) { jump = value; }

    // The drop action (G on a desktop, which has no grips) puts it back.
    public override void InputDrop(bool value, UdonInputEventArgs args)
    {
        if (value && held != 0) PutBack();
    }

    private int Hand(UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return 1;   // a desktop's click is the right hand's
        return args.handType == HandType.LEFT ? 0 : 1;
    }

    public override void InputUse(bool value, UdonInputEventArgs args) { trigger[Hand(args)] = value; }

    public override void InputGrab(bool value, UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        // on a desktop the right mouse button is the other trigger
        if (player.IsUserInVR()) grip[Hand(args)] = value;
        else trigger[0] = value;
    }
}
