using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// Text from a field a visitor types or pastes into (docs/console.md): it goes to a keyboard as
// if typed there, a little at a time, as the guest takes it. At the console's as characters,
// at the display's as key events, where a character no key gives is left out and counted.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPaste : UdonSharpBehaviour
{
    public InputField field;
    public EmuKeyboard keyboard;
    public Text status;

    private string pending = "";
    private int sent, skipped;

    private void Queue(string text)
    {
        if (text == null || text.Length == 0) return;
        text = text.Replace(((char)13).ToString(), "");   // (a pasted line may end in both)
        if (sent >= pending.Length)
        {
            pending = "";
            sent = 0;
        }
        pending += text;
    }

    // The field's text, typed.
    public void Send()
    {
        Queue(field.text);
        field.text = "";
    }

    // And Return after it.
    public void SendLine()
    {
        Queue(field.text + "\n");
        field.text = "";
    }

    // What has not been typed yet is dropped, and the field emptied.
    public void Drop()
    {
        pending = "";
        sent = 0;
        skipped = 0;
        field.text = "";
        Show();
    }

    public int Waiting()
    {
        return pending.Length - sent;
    }

    private void Show()
    {
        if (status != null) status.text = (Waiting() > 0 ? Waiting() + " to type" : "") + (skipped > 0 ? "  " + skipped + " left out" : "");
    }

    void Update()
    {
        if (sent >= pending.Length || keyboard == null) return;
        if (keyboard.rawKeys)
        {
            // key events: four a frame reach the guest, so a character a frame, two where no Shift is in the way
            if (keyboard.Count() > 8) return;
            for (int i = 0; i < 2 && sent < pending.Length; i++)
                if (!keyboard.TypeKey(pending[sent++])) skipped++;
            Show();
            return;
        }
        // the keyboard's queue holds 1,024: it is kept under a quarter of that, for the keys pressed meanwhile
        int room = 256 - keyboard.Count();
        if (room <= 0) return;
        int count = Mathf.Min(room, pending.Length - sent);
        keyboard.TypeText(pending.Substring(sent, count));
        sent += count;
        Show();
    }
}
