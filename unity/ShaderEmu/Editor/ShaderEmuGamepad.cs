using TMPro;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// The game controllers (docs/gamepad.md) are taken with the keys of a holodeck's console
// (ShaderEmuHolodeck.cs). Here: what the board on the wall says of them,
// and the clearing away of the two that lay on the den's desk.
public static partial class ShaderEmuBuilder
{
    // The board's "what you can do", here because the controllers' part of it is the longest.
    public const int DoSize = 25;
    public const string DoText =
        "Press POWER on the computer's tower, right of the desk (or on the control panel). Linux boots in a few " +
        "seconds and starts the desktop.\n\n" +
        "DISPLAY: each hand's beam is the mouse (trigger = left, grip = right, right stick = wheel).\n" +
        "Keyboards on the desk: both hands type, or press 'Use my keyboard' to use your real one. Over the " +
        "display's keyboard: a field to paste text into, and one for a link the computer is to open (a page, a " +
        "picture, a game, a file).\n" +
        "Start menu (or the Win key): games, tools, the Web (a fixed list of sites: VRChat lets a world load " +
        "only addresses it was built with).\n" +
        "CONSOLE: the machine's serial line. Four terminals (the buttons above it), and a field over its " +
        "keyboard to paste text into.\n" +
        "MEMORY: all of the machine's memory at once; what is being written glows.\n\n" +
        "HOLODECKS, through the doorway in the left wall: eight rooms, a seat each. Sit down and a 3D game's " +
        "world is round you; the console at your right hand has the room's keys and a screen, where others " +
        "see your display. Its Shooter and Strategy keys put a GAME CONTROLLER in your hand; Put back, or " +
        "BOTH GRIPS held for a second, takes it away.\n" +
        "White, SHOOTER (Quake, Doom): left stick walks and steps aside, right stick turns and looks, right " +
        "trigger fires, left trigger jumps or opens, right grip next weapon, left grip Esc, jump Enter.\n" +
        "Blue, STRATEGY (Red Alert, Command & Conquer): right stick moves the pointer, the triggers are the " +
        "mouse's buttons, left stick scrolls the map, right grip with the right stick is the wheel, left grip " +
        "Esc, jump Enter.";

    // The two controllers that lay on the den's desk, their names and their station: gone.
    static void NoDeskControllers(Transform world, Transform computer)
    {
        foreach (string old in new[] { "Gamepad", "Shooter controller", "Strategy controller", "Gamepad station" })
            if (world.Find(old) != null) Object.DestroyImmediate(world.Find(old).gameObject);
        foreach (string old in new[] { "Gamepad dock", "Gamepad panel", "Shooter controller dock", "Strategy controller dock",
                                       "Shooter controller name", "Strategy controller name" })
            if (computer != null && computer.Find(old) != null) Object.DestroyImmediate(computer.Find(old).gameObject);
        // the board says where they are now: its text in a scene built before this
        GameObject board = GameObject.Find("About");
        Transform does = board != null ? board.transform.Find("Do") : null;
        if (does != null)
        {
            TextMeshProUGUI text = does.GetComponent<TextMeshProUGUI>();
            text.text = DoText;
            text.fontSize = DoSize;
            EditorUtility.SetDirty(text);
        }
    }
}
