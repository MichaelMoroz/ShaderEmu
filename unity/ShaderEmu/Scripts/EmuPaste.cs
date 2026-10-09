using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// Text for the console from a field a visitor types or pastes into (docs/console.md): it goes
// to the console's keyboard as if typed there, a little at a time, as the guest takes it.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPaste : UdonSharpBehaviour
{
    public InputField field;
    public EmuKeyboard keyboard;
    public Text status;

    private string pending = "";
    private int sent;

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
        field.text = "";
        Show();
    }

    public int Waiting()
    {
        return pending.Length - sent;
    }

    private void Show()
    {
        if (status != null) status.text = Waiting() > 0 ? Waiting() + " to type" : "";
    }

    void Update()
    {
        if (sent >= pending.Length || keyboard == null) return;
        // the keyboard's queue holds 1,024: it is kept under a quarter of that, for the keys pressed meanwhile
        int room = 256 - keyboard.Count();
        if (room <= 0) return;
        int count = Mathf.Min(room, pending.Length - sent);
        keyboard.TypeText(pending.Substring(sent, count));
        sent += count;
        Show();
    }
}
