using UdonSharp;
using UnityEngine;

// One key of an on-screen keyboard. Its button calls Press.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuKey : UdonSharpBehaviour
{
    public EmuKeyboard keyboard;
    public int linuxCode;   // linux/input-event-codes.h
    public int normal;      // character, or 256 and up for keys without one (EmuKeyboard)
    public int shifted;

    public void Press()
    {
        keyboard.VirtualKey(linuxCode, normal, shifted);
    }
}
