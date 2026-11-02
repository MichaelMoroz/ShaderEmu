using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// The game controller (docs/world/gamepad.md): one behaviour a client. A visitor in a headset has it
// from sitting down in a holodeck's seat until they stand up; their sticks, triggers and grips
// are then the guest's keys and pointer, mapped for the program whose window has the guest's
// keyboard (EmuMachine.gameId). There is no model of it: a notice over the hand says what the
// controls are now.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuGamepad : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuKeyboard keys;           // the display's keyboard: key events for the guest
    public EmuPointer pointer;         // the beams, which answer only the seat's console meanwhile
    public EmuShareHub hub;
    public EmuHolodeck holodeck;
    public Transform notice;           // over the hand, the holder's own: what the controls are and how to stand up
    public TextMeshProUGUI noticeLabel;
    public Transform noticeBar;        // fills while both grips are held
    public float pointerSpeed = 700f;  // the pointer's: display pixels a second at full stick

    // Linux key codes (docs/machine/input.md)
    private const int Esc = 1, Tab = 15, W = 17, E = 18, Enter = 28, Ctrl = 29, A = 30, S = 31, D = 32, Z = 44, X = 45, Comma = 51, Dot = 52,
                      Slash = 53, Space = 57, Up = 103, Left = 105, Right = 106, Down = 108;
    private const int Pressed = 65536;
    // what a mapping can hold down: indices into `down`
    private const int Forward = 0, Back = 1, StrafeLeft = 2, StrafeRight = 3, TurnLeft = 4, TurnRight = 5, LookUp = 6,
                      LookDown = 7, Fire = 8, Jump = 9, Next = 10, Menu = 11, Accept = 12, Held = 13;
    // the mappings: a pointer (the desktop, the strategy games), two sticks, blocks, a pad, the hands
    private const int ForPointer = 0, ForShooter = 1, ForBlocks = 2, ForPad = 3, ForHands = 4;
    // the guest's programs that have a mapping of their own (linux/apps/gameid.h)
    private const int GameDoom = 1, GameQuake = 2, GameClassiCube = 3, GameNes = 6, GameQuake3 = 7, GamePhysics = 8, GameHalfLife = 9, GameJk2 = 10;

    private bool held;                      // the controller is in this visitor's hands
    private int scheme, mappedGame;
    private VRCStation seat;                // the seat it was taken in
    private float moveX, moveY, lookX, lookY;
    private bool[] trigger = new bool[2];   // left, right
    private bool[] grip = new bool[2];
    private bool jump;
    private bool[] down = new bool[Held];
    private int[] code = new int[Held];
    private float px, py;                   // where the pointer is
    private float turned;                   // notches of the wheel not sent yet
    private float phase;                    // where in a pulse a slight turn is
    private float bothGrips;                // seconds both grips have been held
    private float[] alone = new float[2];   // seconds a grip has been held without the other; -1: it was with it
    private const float GripWait = 0.15f;
    private float saidAt;

    void Start()
    {
        Map(ForPointer);
        if (notice != null) notice.gameObject.SetActive(false);
    }

    // The keys a mapping holds down: what was held under the last one comes up first.
    private void Map(int to)
    {
        ReleaseAll();
        // (a button of the pointer's that a trigger held goes up with the mapping that pressed it)
        if (held && (scheme == ForPointer || scheme == ForBlocks)) machine.SetPointerButtons(0);
        scheme = to;
        mappedGame = machine.gameId;
        for (int i = 0; i < Held; i++) code[i] = 0;
        // (the hands' grips take things: none of them is a key; ClassiCube's left grip is its
        // inventory, by the words, and its right grip is Esc, its pause menu: Enter was chat)
        code[Menu] = to == ForHands || to == ForBlocks ? 0 : Esc;
        code[Accept] = to == ForBlocks ? Esc : Enter;
        if (to == ForShooter)
        {
            code[Forward] = Up;
            code[Back] = Down;
            code[StrafeLeft] = Comma;
            code[StrafeRight] = Dot;
            code[TurnLeft] = Left;
            code[TurnRight] = Right;
            // (no keys to look up and down: the aim's rise is the hand's, docs/world/holodeck.md)
            // (Half-Life fires by the trigger's word and its Ctrl ducks; its use is E)
            bool halfLife = mappedGame == GameHalfLife;
            code[Fire] = halfLife ? 0 : Ctrl;
            code[Jump] = Space;
            code[Next] = halfLife ? E : Slash;
        }
        else if (to == ForBlocks)
        {
            code[Forward] = W;
            code[Back] = S;
            code[StrafeLeft] = A;
            code[StrafeRight] = D;
            code[TurnLeft] = Left;
            code[TurnRight] = Right;
            code[LookUp] = Up;
            code[LookDown] = Down;
            code[Jump] = Space;
        }
        else if (to == ForPad)
        {
            code[Forward] = Up;
            code[Back] = Down;
            code[TurnLeft] = Left;
            code[TurnRight] = Right;
            code[Fire] = X;       // A
            code[Jump] = Z;       // B
            code[Next] = Tab;     // Select; Enter is Start
        }
        else if (to == ForHands)
        {
            // no keys but Enter: the program reads the hands and the sticks from the host's words
        }
        else
        {
            code[Forward] = Up;   // the arrow keys: the strategy games scroll their maps by them
            code[Back] = Down;
            code[TurnLeft] = Left;
            code[TurnRight] = Right;
        }
        saidAt = Time.time;
    }

    private int SchemeOf(int game)
    {
        return game == GameDoom || game == GameQuake || game == GameQuake3 || game == GameHalfLife || game == GameJk2 ? ForShooter : game == GameClassiCube ? ForBlocks : game == GameNes ? ForPad
             : game == GamePhysics ? ForHands : ForPointer;
    }

    public string SchemeName()
    {
        int game = machine.gameId;
        return scheme == ForShooter ? (game == GameDoom ? "SHOOTER (Doom)" : game == GameQuake3 ? "SHOOTER (Quake III)" : game == GameHalfLife ? "SHOOTER (Half-Life)" : game == GameJk2 ? "SHOOTER (Jedi Outcast)" : "SHOOTER (Quake)") : scheme == ForBlocks ? "BLOCKS (ClassiCube)"
             : scheme == ForPad ? "PAD (NES)" : scheme == ForHands ? "HANDS (Physics)" : "POINTER";
    }

    // ---- sitting down and standing up (EmuHolodeck) ----

    public void Take(VRCStation inSeat)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || inSeat == null) return;
        held = true;
        seat = inSeat;
        // (the sticks are the game's now: the seat keeps its sitter, EmuHolodeck sees to that)
        pointer.away = true;
        px = machine.OwnWidth() * 0.5f;
        py = machine.OwnHeight() * 0.5f;
        bothGrips = -1f;   // (grips held now must let go before they can stand the sitter up)
        Map(SchemeOf(machine.gameId));
        Tell(1);
    }

    public void PutBack()
    {
        if (!held) return;
        held = false;
        ReleaseAll();
        machine.SetPointerButtons(0);
        pointer.away = false;
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

    public int Holding() { return held ? 1 : 0; }

    // The sticks as the 3D games read them (docs/world/holodeck.md, "Played from the seat"): a signed
    // byte an axis, 127 pushed home. Low: the left stick, right and forward; high: the right one.
    private int Axis(float v) { return Mathf.Clamp(Mathf.RoundToInt(v * 127f), -127, 127) & 255; }
    private bool InMenu() { return holodeck != null && holodeck.Flat(); }
    private bool Sticks() { return held && (scheme == ForShooter || scheme == ForBlocks || scheme == ForHands) && !InMenu(); }
    public int StickLow() { return Sticks() ? Axis(moveX) | (Axis(moveY) << 8) : 0; }
    public int StickHigh() { return Sticks() ? Axis(lookX) | (Axis(lookY) << 8) : 0; }
    public bool Jumping() { return held && jump; }

    // ---- the guest's keys ----

    private void Set(int which, bool on)
    {
        if (down[which] == on || code[which] == 0) return;
        down[which] = on;
        // (this visitor's own machine's, whatever the den's wall shows: as the pointer is)
        if (machine.inputAway) machine.RemoteKey(on ? code[which] | Pressed : code[which]);
        else keys.PushEvent(on ? code[which] | Pressed : code[which]);
    }

    private void ReleaseAll()
    {
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

    // A grip's key: held alone a moment, or tapped alone (the first of both grips, standing up, is neither).
    private bool Alone(int h)
    {
        if (grip[h] && grip[1 - h]) { alone[h] = -1f; return false; }
        if (grip[h])
        {
            if (alone[h] >= 0f) alone[h] += Time.deltaTime;
            return alone[h] > GripWait;
        }
        bool tap = alone[h] > 0f && alone[h] <= GripWait;
        alone[h] = 0f;
        return tap;
    }

    // The pointer where the right stick has moved it, with the triggers as its buttons.
    private void Point(bool move, bool left, bool right)
    {
        int w = machine.OwnWidth(), h = machine.OwnHeight();
        if (w <= 0 || h <= 0) return;
        if (move)
        {
            // faster the further the stick is pushed: fine near the middle
            float fx = Mathf.Abs(lookX) < 0.15f ? 0f : lookX * Mathf.Abs(lookX);
            float fy = Mathf.Abs(lookY) < 0.15f ? 0f : lookY * Mathf.Abs(lookY);
            px = Mathf.Clamp(px + fx * pointerSpeed * Time.deltaTime, 0f, w - 1);
            py = Mathf.Clamp(py - fy * pointerSpeed * Time.deltaTime, 0f, h - 1);
        }
        // (this visitor's own machine's, whatever the den's wall shows)
        machine.SetPointer((int)px, (int)py, (left ? 1 : 0) | (right ? 2 : 0));
        machine.pointerOwn = true;
    }

    void Update()
    {
        if (!held) return;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        // both grips together, for a second: out of the seat (not until both were let go once)
        if (grip[0] && grip[1])
        {
            if (bothGrips >= 0f) bothGrips += Time.deltaTime;
        }
        else if (!grip[0] && !grip[1]) bothGrips = 0f;
        if (bothGrips > 1f)
        {
            if (holodeck != null) holodeck.StandUp();
            else PutBack();
            return;
        }
        int wanted = SchemeOf(machine.gameId);
        if (wanted != scheme || machine.gameId != mappedGame) Map(wanted);
        Notice(player);

        // a hand whose beam is on the seat's console or screen presses that, not the game
        bool fire = trigger[1] && !pointer.Landed(1), other = trigger[0] && !pointer.Landed(0);
        bool leftGrip = Alone(0) && !pointer.Landed(0), rightGrip = Alone(1) && !pointer.Landed(1);
        phase = (phase + Time.deltaTime * 10f) % 1f;
        Set(Menu, leftGrip);
        if (scheme == ForShooter)
        {
            // (the sticks are the game's own, as sticks: StickLow, StickHigh; in a menu the left
            // one is the arrow keys)
            bool menu = InMenu();
            Set(Forward, menu && moveY > 0.5f);
            Set(Back, menu && moveY < -0.5f);
            Set(TurnLeft, menu && moveX < -0.5f);
            Set(TurnRight, menu && moveX > 0.5f);
            // (no trigger jumps: the jump button does, as the host's words say: Jumping)
            Set(Fire, fire);
            Set(Next, rightGrip);
            Set(Accept, menu && jump);
        }
        else if (scheme == ForBlocks)
        {
            Set(Jump, jump);
            Set(Accept, rightGrip);
            // (the right trigger digs and the left one builds, each by its own hand's beam: the
            // game reads the triggers in the host's words, EmuHolodeck.Tell; a menu's pointer is the beam)
        }
        else if (scheme == ForPad)
        {
            Set(Forward, moveY > 0.5f);
            Set(Back, moveY < -0.5f);
            Set(TurnLeft, moveX < -0.5f);
            Set(TurnRight, moveX > 0.5f);
            Set(Fire, fire);
            Set(Jump, other);
            Set(Next, rightGrip);
            Set(Accept, jump);
        }
        else if (scheme == ForHands)
        {
            // (triggers, grips and sticks are told as they are: EmuHolodeck.Tell, StickLow, StickHigh)
            Set(Accept, jump);
        }
        else
        {
            // (a beam on the room's screen has the pointer: EmuPointer)
            if (pointer.OnScreen(0) || pointer.OnScreen(1)) { px = machine.pointerX; py = machine.pointerY; }
            else Point(!rightGrip, fire, other);
            // the right grip held: the right stick is the wheel
            turned = rightGrip && Mathf.Abs(lookY) > 0.25f ? turned + lookY * 12f * Time.deltaTime : 0f;
            int notches = (int)turned;
            if (notches != 0)
            {
                turned -= notches;
                machine.Wheel(notches);
            }
            Set(Forward, moveY > 0.5f);
            Set(Back, moveY < -0.5f);
            Set(TurnLeft, moveX < -0.5f);
            Set(TurnRight, moveX > 0.5f);
            Set(Accept, jump);
        }
    }

    // Over the right hand, turned to the holder's eyes: for ten seconds after sitting down or a
    // change of the controls, and whenever a grip is held, with a bar that fills until they stand.
    private void Notice(VRCPlayerApi player)
    {
        if (notice == null) return;
        bool show = Time.time - saidAt < 10f || grip[0] || grip[1];
        if (notice.gameObject.activeSelf != show) notice.gameObject.SetActive(show);
        if (!show) return;
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        Vector3 at = player.GetTrackingData(VRCPlayerApi.TrackingDataType.RightHand).position + Vector3.up * 0.16f;
        notice.SetPositionAndRotation(at, Quaternion.LookRotation(at - head.position, Vector3.up));
        if (noticeLabel != null)
        {
            string says = "Controls: " + SchemeName() + "\nhold BOTH GRIPS to stand up";
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

    private int Hand(UdonInputEventArgs args)
    {
        return args.handType == HandType.LEFT ? 0 : 1;
    }

    public override void InputUse(bool value, UdonInputEventArgs args) { trigger[Hand(args)] = value; }
    public override void InputGrab(bool value, UdonInputEventArgs args) { grip[Hand(args)] = value; }
}
