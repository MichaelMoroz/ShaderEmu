using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using TMPro;
using VRC.SDKBase;

// A keyboard of the machine: on-screen keys pressed by the players' beams (EmuPointer; one per
// hand, both at once, a key down for as long as the trigger is), and the player's real
// keyboard while captured.
// rawKeys = false: the console's keyboard, a queue of characters for the UART.
// rawKeys = true: the keyboard device (docs/input.md), a queue of key events: a Linux key
// code, plus 65536 while pressed. Its keys stay down for as long as they are held.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuKeyboard : UdonSharpBehaviour
{
    public bool rawKeys;
    public EmuKeyboard other;        // the other keyboard: only one captures the real one
    public TextMeshProUGUI captureLabel;
    public TextMeshProUGUI modifierLabel;

    // The real keyboard's keys (filled in by the scene builder): Unity KeyCode, Linux code,
    // character, character with shift.
    public int[] hostKeys;
    public int[] hostLinux;
    public int[] hostNormal;
    public int[] hostShifted;

    // The on-screen keys: a rectangle each, in the panel's units from its top left corner.
    public float panelWidth, panelHeight;
    public float[] keyX, keyY, keyW, keyH;
    public int[] keyLinux, keyNormal, keyShifted;
    public Image[] keyPlate;
    public Color plateColour, hoverColour, pressedColour, latchedColour;

    public const int KeyUp = 256, KeyDown = 257, KeyRight = 258, KeyLeft = 259, KeyHome = 260,
                     KeyEnd = 261, KeyDelete = 262, KeyPageUp = 263, KeyPageDown = 264, KeyNone = 511;
    public const int LinuxCapture = 1000;   // not a key: the "Use my keyboard" plate
    private const int LinuxShift = 42, LinuxRightShift = 54, LinuxCtrl = 29, LinuxRightCtrl = 97, LinuxAlt = 56,
                      LinuxRightAlt = 100, LinuxCaps = 58;
    private const int Pressed = 65536;

    private int[] queue = new int[1024];
    private int head, tail;

    // shift, ctrl, alt: latched by a press, and whether that press has been used by another key
    private bool[] latched = new bool[3];
    private bool[] fresh = new bool[3];
    private bool[] used = new bool[3];
    private bool caps;
    private int[] hover = new int[2];    // per hand: the key under its beam, or -1
    private int[] held = new int[2];     // per hand: the key it holds down, or -1
    private bool started;

    private bool capturing;
    private int repeatKey = -1;      // real keyboard, index into hostKeys
    private float repeatAt;
    private int repeatScreen = -1;   // on-screen key held on the console keyboard
    private float repeatScreenAt;

    private void Init()
    {
        if (started) return;
        started = true;
        hover[0] = hover[1] = held[0] = held[1] = -1;
    }

    void Start()
    {
        Init();
    }

    public int Count()
    {
        return (tail - head) & 1023;
    }

    public int Pop()
    {
        int v = queue[head];
        head = (head + 1) & 1023;
        return v;
    }

    private void Push(int v)
    {
        int next = (tail + 1) & 1023;
        if (next == head) return;   // full: the key is dropped
        queue[tail] = v;
        tail = next;
    }

    // An event that is not a key of this keyboard: a notch of the wheel (docs/input.md).
    public void PushEvent(int e)
    {
        Push(e);
    }

    public void Flush()
    {
        head = tail;
    }

    // Types text at the console, as if on this keyboard.
    public void TypeText(string text)
    {
        for (int i = 0; i < text.Length; i++) Push(text[i] == '\n' ? '\r' : text[i]);
    }

    // 0 shift, 1 ctrl, 2 alt; -1 for any other key.
    private int Modifier(int linux)
    {
        if (linux == LinuxShift || linux == LinuxRightShift) return 0;
        if (linux == LinuxCtrl || linux == LinuxRightCtrl) return 1;
        if (linux == LinuxAlt || linux == LinuxRightAlt) return 2;
        return -1;
    }

    private int ModifierCode(int which)
    {
        return which == 0 ? LinuxShift : which == 1 ? LinuxCtrl : LinuxAlt;
    }

    private static bool IsModifier(int linux)
    {
        return linux == LinuxShift || linux == LinuxRightShift || linux == LinuxCtrl || linux == LinuxRightCtrl
            || linux == LinuxAlt || linux == LinuxRightAlt || linux == LinuxCaps;
    }

    // A key as the bytes a terminal sends for it.
    private void PushChar(int normal, int shifted, bool withShift, bool withCtrl, bool withAlt)
    {
        int c = withShift ? shifted : normal;
        if (caps && c < 256)
        {
            if (c >= 'a' && c <= 'z') c -= 32;
            else if (c >= 'A' && c <= 'Z') c += 32;
        }
        if (c == KeyNone) return;
        if (c >= 256)
        {
            Push(27);
            Push('[');
            if (c == KeyUp) Push('A');
            else if (c == KeyDown) Push('B');
            else if (c == KeyRight) Push('C');
            else if (c == KeyLeft) Push('D');
            else if (c == KeyHome) Push('H');
            else if (c == KeyEnd) Push('F');
            else
            {
                Push(c == KeyDelete ? '3' : c == KeyPageUp ? '5' : '6');
                Push('~');
            }
            return;
        }
        if (withCtrl && c >= 0x40 && c < 0x80) c &= 0x1f;
        if (withAlt) Push(27);
        Push(c);
    }

    // ---- the on-screen keys ----

    // The key at a point of the world on the panel, or -1.
    public int KeyAt(Vector3 point)
    {
        Vector3 local = transform.InverseTransformPoint(point);
        float x = local.x + panelWidth * 0.5f, y = panelHeight * 0.5f - local.y;
        for (int i = 0; i < keyX.Length; i++)
            if (x >= keyX[i] && y >= keyY[i] && x < keyX[i] + keyW[i] && y < keyY[i] + keyH[i]) return i;
        return -1;
    }

    public void SetHover(int hand, int key)
    {
        Init();
        if (hover[hand] == key) return;
        hover[hand] = key;
        Paint();
    }

    private bool Holding(int which)
    {
        for (int h = 0; h < 2; h++)
            if (held[h] >= 0 && Modifier(keyLinux[held[h]]) == which) return true;
        return false;
    }

    private void Unlatch(int which)
    {
        if (!latched[which]) return;
        latched[which] = false;
        if (rawKeys) Push(ModifierCode(which));
    }

    // A beam's trigger went down on a key.
    public void Press(int hand, int key)
    {
        Init();
        if (key < 0 || held[hand] >= 0 || held[1 - hand] == key) return;
        int linux = keyLinux[key];
        if (linux == LinuxCapture)
        {
            ToggleCapture();
            return;
        }
        held[hand] = key;
        int which = Modifier(linux);
        if (linux == LinuxCaps)
        {
            caps = !caps;
            if (rawKeys) { Push(linux | Pressed); Push(linux); }   // a key like any other to the guest
        }
        else if (which >= 0)
        {
            // a first press latches it for the next key; a second one lets go
            fresh[which] = !latched[which];
            used[which] = false;
            if (!latched[which])
            {
                latched[which] = true;
                if (rawKeys) Push(ModifierCode(which) | Pressed);
            }
        }
        else
        {
            for (int m = 0; m < 3; m++)
                if (Holding(m)) used[m] = true;
            if (rawKeys)
            {
                Push(linux | Pressed);
            }
            else
            {
                PushChar(keyNormal[key], keyShifted[key], latched[0], latched[1], latched[2]);
                repeatScreen = key;
                repeatScreenAt = Time.time + 0.4f;
            }
        }
        ShowModifiers();
        Paint();
    }

    // The trigger came up again.
    public void Release(int hand)
    {
        Init();
        int key = held[hand];
        if (key < 0) return;
        held[hand] = -1;
        int linux = keyLinux[key], which = Modifier(linux);
        if (which >= 0)
        {
            // held while another key was typed, or pressed to let go: it ends here
            if (!fresh[which] || used[which]) Unlatch(which);
        }
        else if (linux != LinuxCaps)
        {
            if (rawKeys) Push(linux);
            if (repeatScreen == key) repeatScreen = -1;
            // a latched shift, ctrl or alt holds for one key, unless a hand is still on it
            for (int m = 0; m < 3; m++)
                if (!Holding(m)) Unlatch(m);
        }
        ShowModifiers();
        Paint();
    }

    private void Paint()
    {
        if (keyPlate == null) return;
        for (int i = 0; i < keyPlate.Length; i++)
        {
            int which = Modifier(keyLinux[i]);
            bool down = held[0] == i || held[1] == i;
            bool lit = (which >= 0 && latched[which]) || (keyLinux[i] == LinuxCaps && caps)
                    || (keyLinux[i] == LinuxCapture && capturing);
            keyPlate[i].color = down ? pressedColour : lit ? latchedColour
                              : (hover[0] == i || hover[1] == i) ? hoverColour : plateColour;
        }
    }

    private void ShowModifiers()
    {
        if (modifierLabel == null) return;
        modifierLabel.text = (latched[0] ? "SHIFT " : "") + (latched[1] ? "CTRL " : "") + (latched[2] ? "ALT " : "") + (caps ? "CAPS" : "");
    }

    // ---- the player's real keyboard ----

    public void ToggleCapture()
    {
        SetCapture(!capturing);
    }

    public void SetCapture(bool on)
    {
        Init();
        if (on == capturing) return;
        if (on && other != null) other.SetCapture(false);
        capturing = on;
        repeatKey = -1;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null) player.Immobilize(on);   // or typing WASD walks away
        if (captureLabel != null) captureLabel.text = on ? "Typing here (Insert: stop)" : "Use my keyboard";
        Paint();
    }

    void Update()
    {
        // the console has no key repeat of its own: an on-screen key held there repeats here
        if (repeatScreen >= 0 && Time.time >= repeatScreenAt)
        {
            PushChar(keyNormal[repeatScreen], keyShifted[repeatScreen], latched[0], latched[1], latched[2]);
            repeatScreenAt = Time.time + 0.04f;
        }
        if (!capturing) return;
        if (Input.GetKeyDown(KeyCode.Insert))
        {
            SetCapture(false);
            return;
        }
        bool withShift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
        bool withCtrl = Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl);
        bool withAlt = Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt);
        for (int i = 0; i < hostKeys.Length; i++)
        {
            KeyCode key = (KeyCode)hostKeys[i];
            if (key == KeyCode.None || key == KeyCode.Insert) continue;
            if (rawKeys)
            {
                if (Input.GetKeyDown(key)) Push(hostLinux[i] | Pressed);
                if (Input.GetKeyUp(key)) Push(hostLinux[i]);
            }
            else if (!IsModifier(hostLinux[i]) && Input.GetKeyDown(key))
            {
                PushChar(hostNormal[i], hostShifted[i], withShift, withCtrl, withAlt);
                repeatKey = i;
                repeatAt = Time.time + 0.4f;
            }
        }
        if (!rawKeys && repeatKey >= 0)
        {
            if (!Input.GetKey((KeyCode)hostKeys[repeatKey])) repeatKey = -1;
            else if (Time.time >= repeatAt)
            {
                PushChar(hostNormal[repeatKey], hostShifted[repeatKey], withShift, withCtrl, withAlt);
                repeatAt = Time.time + 0.04f;
            }
        }
    }
}
