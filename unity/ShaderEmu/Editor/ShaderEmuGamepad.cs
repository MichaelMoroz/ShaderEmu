using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// The game controller (docs/world/gamepad.md) is a holodeck sitter's own, from sitting down
// (ShaderEmuHolodeck.cs). Here: what the board on the wall says of it,
// and the clearing away of the two that lay on the den's desk.
public static partial class ShaderEmuBuilder
{
    // The board's "what you can do", here because the controllers' part of it is the longest.
    public const int DoSize = 21, DoHigh = 690, HowAt = 1370;   // the board's left column under it begins at HowAt
    public const string DoText =
        "Press POWER on the computer's tower, right of the desk (or on the control panel). Your machine " +
        "boots Linux in a few seconds and starts the desktop.\n\nDISPLAY: each hand's beam is the mouse " +
        "(trigger = left, grip = right, right stick = wheel).\nKeyboards on the desk: both hands type, or " +
        "press 'Use my keyboard' to use your real one. Over the display's keyboard: a field to paste text " +
        "into, and one for a link the computer is to open (a page, a picture, a game, a file).\nThe desktop's " +
        "icons and the Start menu (or the Win key): games (Doom, Quake, Quake III Arena, Half-Life: Uplink, " +
        "Red Alert, Command & Conquer, Age of Empires, Jedi Outcast, OpenTTD, Micropolis, chess, a NES, a block world), " +
        "a music and a video player, " +
        "pictures, the Web (a fixed list of sites, or a link you paste), an IDE for C with the machine's own " +
        "compiler and debugger, and Settings (the Windows 7 or the classic look, and the windows' colour). A right click is a menu; " +
        "icons and files are dragged.\nCONSOLE: the machine's serial line. Four terminals (the buttons above " +
        "it), and a field over its keyboard to paste text into.\nMEMORY: all of the machine's memory at once; " +
        "what is being written glows.\n\nHOLODECKS, through the doorway in the left wall: eight rooms, a seat " +
        "each. Sit down and a 3D game's world is round you; the console at your right hand has the room's " +
        "keys and a screen, where others see your display. In a headset your controllers are the game's " +
        "while you sit, set for the game whose window is in front; BOTH GRIPS held for a second, or the " +
        "console's Exit, stands you up.\nSHOOTER (Quake, Quake III, Doom, Half-Life): left stick walks and steps aside, " +
        "right stick turns and looks, right trigger fires, left trigger jumps or opens, right grip next " +
        "weapon (in Half-Life: use), left grip Esc, jump Enter.\nPOINTER (the desktop, Red Alert, Command & Conquer, Age of Empires): right stick " +
        "moves the pointer, the triggers are the mouse's buttons, left stick scrolls the map, right grip " +
        "with the right stick is the wheel.\nPAD (the NES): left stick the pad, right trigger A, left trigger " +
        "B, jump Start, right grip Select.\nBLOCKS (the block world): left stick walks, right stick looks, " +
        "right trigger digs, left builds.\nHANDS (the physics demo): a hand's trigger or grip takes the box " +
        "it is at or points at, and lets it fly; left stick walks, right stick turns, up fires a box, down " +
        "begins again, jump the next scene.\n\nCLASSROOM: twelve computers, and one in each side room off the corridor by the " +
        "lift (N13 to N15). Press POWER on a free one and your " +
        "machine starts there; the slider on its tower sets its speed (Snail to Turbo), EXIT gives the place " +
        "up. PENS under this board draw in the air.";

    // The board's words written again into a scene that has a board, each at the largest size at
    // which it fits its box, and the plate lettered from them (no bake: the plate keeps its place).
    [MenuItem("ShaderEmu/Write the board's words again in the open scene")]
    public static void WriteBoardAgain()
    {
        GameObject board = GameObject.Find("About");
        if (board == null) { Debug.LogError("[ShaderEmu] no board in the open scene"); return; }
        foreach (var words in new[] { ("What", WhatText, 33f), ("Do", DoText, (float)DoSize), ("Credits", CreditsText, 27f) })
        {
            Transform at = board.transform.Find(words.Item1);
            TextMeshProUGUI text = at != null ? at.GetComponent<TextMeshProUGUI>() : null;
            if (text == null) { Debug.LogWarning("[ShaderEmu] the board has no " + words.Item1); continue; }
            text.text = words.Item2;
            text.fontSize = words.Item3;
            float room = text.rectTransform.rect.height;
            while (text.fontSize > 12 && text.GetPreferredValues(words.Item2, text.rectTransform.rect.width, 0).y > room) text.fontSize -= 0.5f;
            Debug.Log("[ShaderEmu] the board's " + words.Item1 + ": size " + text.fontSize + ", " +
                      Mathf.CeilToInt(text.GetPreferredValues(words.Item2, text.rectTransform.rect.width, 0).y) + " of " + Mathf.RoundToInt(room));
            EditorUtility.SetDirty(text);
        }
        LetterBoardAgain();
    }

    // A controller in hand was a model once, two on every player's object: there is none now
    // (the stand's line and the notice over the hand say what is held).
    static void NoControllerModels(Transform world)
    {
        foreach (EmuShare share in world.GetComponentsInChildren<EmuShare>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(share);
            for (int i = share.transform.childCount - 1; i >= 0; i--) Object.DestroyImmediate(share.transform.GetChild(i).gameObject);
            UdonSharpEditorUtility.CopyProxyToUdon(share);
            EditorUtility.SetDirty(share);
        }
        foreach (EmuHolodeck deck in world.GetComponentsInChildren<EmuHolodeck>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(deck);
            UdonSharpEditorUtility.CopyProxyToUdon(deck);
            EditorUtility.SetDirty(deck);
        }
    }

    [MenuItem("ShaderEmu/Take the controllers' models out of the open scene")]
    public static void RemoveControllerModels()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        NoControllerModels(world);
        NoDeskControllers(world, world.Find("Computer"));   // (and the board's words, which named their colours)
        EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        Debug.Log("[ShaderEmu] no controller models (the scene is not saved)");
    }

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
            // (and the room for it, in a scene whose board was laid out for fewer words)

            text.rectTransform.sizeDelta = new Vector2(text.rectTransform.sizeDelta.x, DoHigh);
            foreach (string name in new[] { "How head", "How" })
            {
                Transform below = board.transform.Find(name);
                if (below == null) continue;
                RectTransform rect = below.GetComponent<RectTransform>();
                rect.anchoredPosition = new Vector2(rect.anchoredPosition.x, -(name == "How" ? HowAt + 50 : HowAt));
                if (name == "How") rect.sizeDelta = new Vector2(rect.sizeDelta.x, 270);
                EditorUtility.SetDirty(rect);
            }
            EditorUtility.SetDirty(text);
        }
    }
}
