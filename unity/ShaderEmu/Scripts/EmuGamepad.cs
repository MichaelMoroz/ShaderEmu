using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// A game controller (docs/gamepad.md): a model on the desk a visitor takes into their hand.
// While they have it, their sticks, triggers and grips are the guest's keys and pointer, and
// they stand where they are. There are two, one for shooters and one for strategy games.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuGamepad : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuKeyboard keys;           // the display's keyboard: key events for the guest
    public EmuPointer pointer;         // the beams, which rest while the controller is held
    public Transform model;
    public Transform dock;             // where the model lies when nobody has it
    public VRCStation station;         // holds the player still: their sticks are the game's
    public EmuGamepad other;           // the other controller: one at a time
    public int mapping;                // 0 shooter, 1 strategy
    public bool last;                  // the one both grips bring back to the hand
    public float pointerSpeed = 700f;  // strategy: display pixels a second at full stick

    // Linux key codes (docs/input.md)
    private const int Esc = 1, Enter = 28, Ctrl = 29, A = 30, Z = 44, Comma = 51, Dot = 52, Slash = 53, Space = 57,
                      Up = 103, Left = 105, Right = 106, Down = 108;
    private const int Pressed = 65536;
    // what a mapping can hold down: indices into `down`
    private const int Forward = 0, Back = 1, StrafeLeft = 2, StrafeRight = 3, TurnLeft = 4, TurnRight = 5, LookUp = 6,
                      LookDown = 7, Fire = 8, Jump = 9, Next = 10, Menu = 11, Accept = 12, Held = 13;

    private bool held;
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
    }

    // ---- taking it and putting it back ----

    public override void Interact()
    {
        Take();
    }

    public void Take()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (held || player == null) return;
        if (other != null)
        {
            other.PutBack();
            other.last = false;
        }
        last = true;
        held = true;
        DisableInteractive = true;   // (it is in their hand: nothing to click there)
        pointer.away = true;
        // held where they stand: by a station, which also keeps the sticks from turning them, and
        // by being told to stand still, which is all there is should the station not take them
        if (station != null)
        {
            station.transform.SetPositionAndRotation(player.GetPosition(), player.GetRotation());
            station.UseStation(player);
        }
        player.Immobilize(true);
        px = machine.shownWidth * 0.5f;
        py = machine.shownHeight * 0.5f;
        bothGrips = -1f;   // (the grips that took it must let go before they can put it back)
    }

    public void PutBack()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (!held) return;
        held = false;
        DisableInteractive = false;
        ReleaseAll();
        machine.SetPointerButtons(0);
        pointer.away = false;
        if (player != null)
        {
            if (station != null) station.ExitStation(player);
            player.Immobilize(false);
        }
        model.SetPositionAndRotation(dock.position, dock.rotation);
        bothGrips = -1f;
    }

    public override void OnPlayerRespawn(VRCPlayerApi player)
    {
        if (player != null && player.isLocal) PutBack();
    }

    public bool IsHeld() { return held; }

    // ---- the guest's keys ----

    private void Set(int which, bool on)
    {
        if (down[which] == on) return;
        down[which] = on;
        keys.PushEvent(on ? code[which] | Pressed : code[which]);
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
        // both grips together, for a second: back to the desk, or the last one held into the hand
        // again (after either, not until both have been let go)
        if (grip[0] && grip[1])
        {
            if (bothGrips >= 0f) bothGrips += Time.deltaTime;
        }
        else if (!grip[0] && !grip[1]) bothGrips = 0f;
        if (bothGrips > 1f)
        {
            if (held) PutBack();
            else if (last && (other == null || !other.IsHeld())) Take();
            return;
        }
        if (!held) return;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;

        // the model in the hand: the right one in VR, before the chest on a desktop
        if (player.IsUserInVR())
        {
            VRCPlayerApi.TrackingData hand = player.GetTrackingData(VRCPlayerApi.TrackingDataType.RightHand);
            model.SetPositionAndRotation(hand.position, hand.rotation * Quaternion.Euler(0f, 90f, 0f));
        }
        else
        {
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            model.SetPositionAndRotation(head.position + head.rotation * new Vector3(0f, -0.28f, 0.45f), head.rotation * Quaternion.Euler(-60f, 0f, 0f));
        }

        phase = (phase + Time.deltaTime * 10f) % 1f;
        if (mapping == 0)
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
            int w = machine.shownWidth, h = machine.shownHeight;
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
                machine.SetPointer((int)px, (int)py, (trigger[1] ? 1 : 0) | (trigger[0] ? 2 : 0));
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

    // ---- the player's sticks and buttons ----

    public override void InputMoveHorizontal(float value, UdonInputEventArgs args) { moveX = value; }
    public override void InputMoveVertical(float value, UdonInputEventArgs args) { moveY = value; }
    public override void InputLookHorizontal(float value, UdonInputEventArgs args) { lookX = value; }
    public override void InputLookVertical(float value, UdonInputEventArgs args) { lookY = value; }
    public override void InputJump(bool value, UdonInputEventArgs args) { jump = value; }

    // The drop action (G on a desktop, which has no grips) puts it back.
    public override void InputDrop(bool value, UdonInputEventArgs args)
    {
        if (value && held) PutBack();
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
