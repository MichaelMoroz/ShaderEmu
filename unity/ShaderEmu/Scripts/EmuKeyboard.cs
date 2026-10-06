using UdonSharp;
using UnityEngine;
using TMPro;
using VRC.SDKBase;

// A keyboard of the machine: on-screen keys, and the player's real keyboard while captured.
// rawKeys = false: the console's keyboard, a queue of characters for the UART.
// rawKeys = true: the keyboard device (docs/input.md), a queue of key events: a Linux key
// code, plus 65536 while pressed.
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

    public const int KeyUp = 256, KeyDown = 257, KeyRight = 258, KeyLeft = 259, KeyHome = 260,
                     KeyEnd = 261, KeyDelete = 262, KeyPageUp = 263, KeyPageDown = 264, KeyNone = 511;
    private const int LinuxShift = 42, LinuxRightShift = 54, LinuxCtrl = 29, LinuxAlt = 56, LinuxCaps = 58;
    private const int Pressed = 65536;

    private int[] queue = new int[1024];
    private int head, tail;

    private bool shift, ctrl, alt, caps;   // latched by the on-screen keys
    private bool capturing;
    private int repeatKey = -1;
    private float repeatAt;

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

    public void Flush()
    {
        head = tail;
    }

    // Types text at the console, as if on this keyboard.
    public void TypeText(string text)
    {
        for (int i = 0; i < text.Length; i++) Push(text[i] == '\n' ? '\r' : text[i]);
    }

    private static bool IsModifier(int linux)
    {
        return linux == LinuxShift || linux == LinuxRightShift || linux == LinuxCtrl || linux == LinuxAlt || linux == LinuxCaps;
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

    // An on-screen key was clicked.
    public void VirtualKey(int linux, int normal, int shifted)
    {
        if (IsModifier(linux))
        {
            bool on;
            if (linux == LinuxCtrl) { ctrl = !ctrl; on = ctrl; }
            else if (linux == LinuxAlt) { alt = !alt; on = alt; }
            else if (linux == LinuxCaps) { caps = !caps; on = caps; }
            else { shift = !shift; on = shift; linux = LinuxShift; }
            if (rawKeys)
            {
                // caps lock is a key like any other to the guest; the others are held down
                if (linux == LinuxCaps) { Push(linux | Pressed); Push(linux); }
                else Push(on ? (linux | Pressed) : linux);
            }
            ShowModifiers();
            return;
        }
        if (rawKeys)
        {
            Push(linux | Pressed);
            Push(linux);
            if (shift) Push(LinuxShift);
            if (ctrl) Push(LinuxCtrl);
            if (alt) Push(LinuxAlt);
        }
        else
        {
            PushChar(normal, shifted, shift, ctrl, alt);
        }
        // shift, ctrl and alt hold for one key
        shift = false;
        ctrl = false;
        alt = false;
        ShowModifiers();
    }

    private void ShowModifiers()
    {
        if (modifierLabel == null) return;
        modifierLabel.text = (shift ? "SHIFT " : "") + (ctrl ? "CTRL " : "") + (alt ? "ALT " : "") + (caps ? "CAPS" : "");
    }

    public void ToggleCapture()
    {
        SetCapture(!capturing);
    }

    public void SetCapture(bool on)
    {
        if (on == capturing) return;
        if (on && other != null) other.SetCapture(false);
        capturing = on;
        repeatKey = -1;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null) player.Immobilize(on);   // or typing WASD walks away
        if (captureLabel != null) captureLabel.text = on ? "Typing here (Insert: stop)" : "Use my keyboard";
    }

    void Update()
    {
        if (!capturing) return;
        if (Input.GetKeyDown(KeyCode.Insert))
        {
            SetCapture(false);
            return;
        }
        bool withShift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
        bool withCtrl = Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl);
        bool withAlt = Input.GetKey(KeyCode.LeftAlt);
        for (int i = 0; i < hostKeys.Length; i++)
        {
            KeyCode key = (KeyCode)hostKeys[i];
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
        // the console has no key repeat of its own
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
