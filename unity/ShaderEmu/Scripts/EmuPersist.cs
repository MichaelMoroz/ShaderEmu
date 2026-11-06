using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDK3.Persistence;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// The guest's folder /persistence between sessions (docs/system/persist.md): its packed bytes are this
// player's PlayerData in this world. EmuMachine answers the guest's "persist:" addresses from
// here; the sign on the computer has Save, the switch for loading at start, and Clear.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPersist : UdonSharpBehaviour
{
    public EmuMachine machine;
    public Material readMaterial;         // ShaderEmu/PersistRead
    public RenderTexture readTexture;     // 256 x 100: a pixel a word of what the guest hands over
    public TextMeshProUGUI saysLabel, autoLabel, clearLabel;
    public bool persistTest;              // a client tests itself and logs (docs/system/persist.md, "In a real client")

    public const int Limit = 100000;      // VRChat keeps 100 KB a player and world
    private const string BlobKey = "persist.blob", AutoKey = "persist.auto";

    [HideInInspector] public bool reading;      // a readback of the guest's bytes is on its way
    [HideInInspector] public int saveStatus;    // and what came of it, as a fetch's status
    private bool restored;
    private byte[] taken = new byte[256 * 100 * 4];
    private int wanted;
    private float clearUntil;

    public EmuGate gate;

    // a restore while this was off behind the gate: the gate heard it
    void Start()
    {
        if (gate != null && gate.restored && !restored) OnPlayerRestored(Networking.LocalPlayer);
    }

    public override void OnPlayerRestored(VRCPlayerApi player)
    {
        if (!Utilities.IsValid(player) || !player.isLocal || restored) return;
        restored = true;
        Say("");
        if (persistTest) SendCustomEventDelayedSeconds("TestStep", 1f);
    }

    // The test: the machine is turned on, what was kept is read at its console, a file is
    // written there and Save is pressed; then the machine is started again and must read the
    // file back. Every step is a line of this client's log.
    private int testStep;
    private float testAt;

    public void TestStep()
    {
        EmuTerminal terminal = machine.terminal;
        if (testStep == 0)
        {
            Debug.Log("[PersistTest] restored: " + saysLabel.text + " / " + autoLabel.text);
            machine.PowerOn();
            testStep = 1;
        }
        else if (testStep == 1 && machine.displayWidth != 0)
        {
            testStep = 2;
            testAt = Time.time + 10f;   // (the guest loads the folder as its desktop starts)
        }
        else if (testStep == 2 && Time.time >= testAt)
        {
            Debug.Log("[PersistTest] desktop " + machine.displayWidth + " x " + machine.displayHeight + "; the fetch's line: " + machine.fetchLabel.text);
            machine.consoleKeyboard.TypeText("cat /persistence/note.txt; nxreg list\n");
            testStep = 3;
            testAt = Time.time + 8f;
        }
        else if (testStep == 3 && Time.time >= testAt)
        {
            int row = terminal.CursorRow();
            for (int r = Mathf.Max(0, row - 6); r <= row; r++) Debug.Log("[PersistTest] console: " + terminal.LineText(r).TrimEnd());
            machine.consoleKeyboard.TypeText("echo from-vrchat > /persistence/note.txt\n");
            testStep = 4;
            testAt = Time.time + 5f;
        }
        else if (testStep == 4 && Time.time >= testAt)
        {
            Save();
            Debug.Log("[PersistTest] Save pressed: " + saysLabel.text);
            testStep = 5;
            testAt = Time.time + 60f;
        }
        else if (testStep == 5 && (saysLabel.text.StartsWith("Saved.") || Time.time >= testAt))
        {
            Debug.Log("[PersistTest] after the save: " + saysLabel.text + "; the fetch's line: " + machine.fetchLabel.text);
            machine.PowerOn();   // (again: a machine with nothing in its memory)
            testStep = 6;
            testAt = Time.time + 5f;
        }
        else if (testStep == 6 && Time.time >= testAt && machine.displayWidth != 0)
        {
            testStep = 7;
            testAt = Time.time + 10f;
        }
        else if (testStep == 7 && Time.time >= testAt)
        {
            Debug.Log("[PersistTest] started again; the fetch's line: " + machine.fetchLabel.text);
            machine.consoleKeyboard.TypeText("cat /persistence/note.txt\n");
            testStep = 8;
            testAt = Time.time + 6f;
        }
        else if (testStep == 8 && Time.time >= testAt)
        {
            int row = terminal.CursorRow();
            for (int r = Mathf.Max(0, row - 3); r <= row; r++) Debug.Log("[PersistTest] console again: " + terminal.LineText(r).TrimEnd());
            Debug.Log("[PersistTest] done");
            testStep = 9;
        }
        if (testStep < 9) SendCustomEventDelayedSeconds("TestStep", 1f);
    }

    // PlayerData is the player's only once VRChat has brought it.
    public bool Ready() { return restored; }

    public byte[] Blob()
    {
        VRCPlayerApi me = Networking.LocalPlayer;
        if (!restored || !PlayerData.HasKey(me, BlobKey)) return new byte[0];
        return PlayerData.GetBytes(me, BlobKey);
    }

    public bool Auto()
    {
        VRCPlayerApi me = Networking.LocalPlayer;
        return !restored || !PlayerData.HasKey(me, AutoKey) || PlayerData.GetBool(me, AutoKey);
    }

    public void SetAuto(bool on)
    {
        PlayerData.SetBool(AutoKey, on);
        Say("");
    }

    // (a key cannot be taken away again: no bytes is nothing kept)
    public void Forget()
    {
        PlayerData.SetBytes(BlobKey, new byte[0]);
        Say("Cleared.");
    }

    // The guest has left `bytes` of its folder in the fetch's memory: read from the state.
    public void Read(RenderTexture state, int bytes)
    {
        wanted = bytes;
        saveStatus = bytes > Limit ? 413 : 500;
        // (the sign said "Saving..." until something else was pressed)
        if (bytes > Limit) { Say("Not saved: the folder packs to " + ((bytes + 999) / 1000) + " KB."); return; }
        if (bytes < 16 || readMaterial == null) { Say("Not saved."); return; }
        reading = true;
        readMaterial.SetTexture("_State", state);
        VRCGraphics.Blit(state, readTexture, readMaterial);
        VRCAsyncGPUReadback.Request(readTexture, 0, (IUdonEventReceiver)this);
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        reading = false;
        if (request.hasError || !request.TryGetData(taken)) { Say("Not saved: the picture of it was not read."); return; }
        // its own mark and length first (nxpersist.c): anything else is not a folder
        int length = taken[8] | taken[9] << 8 | taken[10] << 16 | taken[11] << 24;
        if (taken[0] != 'S' || taken[1] != 'E' || taken[2] != 'P' || taken[3] != 'D' || length != wanted - 16) { Say("Not saved."); return; }
        byte[] blob = new byte[wanted];
        System.Buffer.BlockCopy(taken, 0, blob, 0, wanted);
        PlayerData.SetBytes(BlobKey, blob);
        saveStatus = 200;
        Say("Saved.");
    }

    // ---- the sign's three buttons ----

    public void Save()
    {
        // (a folder too large is refused by nxpersist on the desktop, which never asks: "Saving..." stood for ever)
        if (machine.AskSave()) Say("Saving: the desktop says how it goes.");
        else Say(machine.Powered() ? "Start the desktop first." : "The computer is off.");
    }

    public void ToggleAuto()
    {
        if (restored) SetAuto(!Auto());
    }

    // Pressed twice within three seconds: what is kept is gone for good.
    public void Clear()
    {
        if (!restored) return;
        if (Time.time < clearUntil)
        {
            clearUntil = 0;
            Forget();
            return;
        }
        clearUntil = Time.time + 3f;
        Say("Press Clear again to forget it.");
    }

    private void Say(string news)
    {
        int kept = restored ? Blob().Length : 0;
        string state = !restored ? "Waiting for VRChat..." : kept == 0 ? "Nothing is kept." : "Kept: " + ((kept + 999) / 1000) + " KB of " + (Limit / 1000) + ".";
        if (saysLabel != null) saysLabel.text = news.Length > 0 ? news + " " + state : state;
        if (autoLabel != null) autoLabel.text = Auto() ? "Load at start: on" : "Load at start: off";
        if (clearLabel != null) clearLabel.text = "Clear";
    }
}
