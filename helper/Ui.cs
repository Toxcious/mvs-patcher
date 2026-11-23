// The helper's look: colours, fonts and the few drawn controls WinForms lacks (cards, a switch, a nav button, a busy bar).
using System.ComponentModel;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

static class Theme
{
    public static readonly Color Bg = Color.FromArgb(0x13, 0x15, 0x1a);
    public static readonly Color Side = Color.FromArgb(0x0d, 0x0f, 0x13);
    public static readonly Color Card = Color.FromArgb(0x1c, 0x1f, 0x26);
    public static readonly Color Border = Color.FromArgb(0x2b, 0x2f, 0x38);
    public static readonly Color Raised = Color.FromArgb(0x2a, 0x2e, 0x37);
    public static readonly Color RaisedHover = Color.FromArgb(0x35, 0x3a, 0x45);
    public static readonly Color Text = Color.FromArgb(0xe9, 0xeb, 0xef);
    public static readonly Color Muted = Color.FromArgb(0x95, 0x9c, 0xa8);
    public static readonly Color Accent = Color.FromArgb(0x7c, 0x5c, 0xff);
    public static readonly Color AccentHover = Color.FromArgb(0x92, 0x77, 0xff);
    public static readonly Color Good = Color.FromArgb(0x3c, 0xcf, 0x91);
    public static readonly Color Warn = Color.FromArgb(0xf5, 0xb5, 0x44);
    public static readonly Color Bad = Color.FromArgb(0xef, 0x5b, 0x5b);
    public static readonly Color Off = Color.FromArgb(0x5d, 0x63, 0x6e);

    public static readonly Font Body = new("Segoe UI", 10f);
    public static readonly Font Small = new("Segoe UI", 9f);
    public static readonly Font Strong = new("Segoe UI Semibold", 10.5f);
    public static readonly Font CardTitle = new("Segoe UI Semibold", 11.5f);
    public static readonly Font PageTitle = new("Segoe UI Semibold", 18f);
    public static readonly Font Brand = new("Segoe UI Semibold", 15f);
    public static readonly Font Code = new(FontFamily.Families.Any(f => f.Name == "Cascadia Mono") ? "Cascadia Mono" : "Consolas", 20f, FontStyle.Bold);
    public static readonly Font Log = new(FontFamily.Families.Any(f => f.Name == "Cascadia Mono") ? "Cascadia Mono" : "Consolas", 9f);

    public static GraphicsPath Round(RectangleF r, float rad)
    {
        var p = new GraphicsPath();
        float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    // Dark title bar (Windows 10 2004 and later; ignored elsewhere).
    public static void DarkTitleBar(Form f)
    {
        int on = 1;
        try { DwmSetWindowAttribute(f.Handle, 20, ref on, 4); } catch { }
    }
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    // The window and taskbar icon, drawn here so there is no image file to ship.
    public static Icon AppIcon()
    {
        using var bmp = new Bitmap(64, 64);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            using var path = Round(new RectangleF(2, 2, 60, 60), 14);
            using var br = new LinearGradientBrush(new Rectangle(0, 0, 64, 64), Accent, Color.FromArgb(0x4a, 0x2f, 0xd6), 45f);
            g.FillPath(br, path);
            using var f = new Font("Segoe UI", 34f, FontStyle.Bold, GraphicsUnit.Pixel);
            var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            g.DrawString("M", f, Brushes.White, new RectangleF(0, 1, 64, 64), sf);
        }
        return Icon.FromHandle(bmp.GetHicon());
    }

    public static Label Text_(string text, Font? font = null, Color? color = null) =>
        new() { Text = text, Font = font ?? Body, ForeColor = color ?? Text, AutoSize = true, Margin = new Padding(0, 2, 0, 2), BackColor = Color.Transparent };

    // A label that wraps to the width it is given (in a VStack).
    public static WrapLabel Wrap(string text, Font? font = null, Color? color = null) =>
        new() { Text = text, Font = font ?? Small, ForeColor = color ?? Muted, Margin = new Padding(0, 2, 0, 2), BackColor = Color.Transparent };

    // The colour actually behind a control (the first ancestor that is not transparent).
    public static Color Under(Control c)
    {
        for (var p = c.Parent; p != null; p = p.Parent) if (p.BackColor.A == 255) return p.BackColor;
        return Card;
    }
}

// A label that wraps its text to its width; its height follows.
sealed class WrapLabel : Label
{
    public WrapLabel() { AutoSize = false; }
    public int HeightFor(int width) =>
        Text.Length == 0 ? 0 : TextRenderer.MeasureText(Text, Font, new Size(Math.Max(width, 1), int.MaxValue), TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl).Height;
    public override Size GetPreferredSize(Size proposed) => new(proposed.Width > 0 ? proposed.Width : Width, HeightFor(proposed.Width > 0 ? proposed.Width : Width));
    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Parent?.PerformLayout(); }
}

// Children one under the other, each as wide as the panel (minus its margins); the panel's height follows its
// children. WinForms' own auto-sizing measures wrapped text at the wrong width, so this does it by hand.
class VStack : Panel
{
    public bool Manual;   // true: children are docked as usual and the height is left alone
    public VStack() { BackColor = Color.Transparent; Margin = new Padding(0); Padding = new Padding(0); }
    public void Add(params Control[] rows) { foreach (var r in rows) Controls.Add(r); }

    static int RowHeight(Control c, int w) => c switch
    {
        WrapLabel l => l.HeightFor(w),
        VStack v => v.HeightFor(w),
        Split sp => sp.HeightFor(w),
        BusyBar b => b.Active ? 4 : 0,
        _ => c.AutoSize || c is FlowLayoutPanel ? c.GetPreferredSize(new Size(w, 0)).Height : c.Height,
    };

    // A row whose height comes out 0 (an empty WrapLabel, an idle BusyBar) takes no room, margins included. Rows
    // are never hidden with Visible: that reads false for everything on a page that is not showing.
    public int HeightFor(int width)
    {
        int y = Padding.Top, w = width - Padding.Horizontal;
        foreach (Control c in Controls) { int h = RowHeight(c, w - c.Margin.Horizontal); if (h > 0) y += h + c.Margin.Vertical; }
        return y + Padding.Bottom;
    }

    protected override void OnLayout(LayoutEventArgs e)
    {
        if (Manual) { base.OnLayout(e); return; }
        int y = Padding.Top, w = Width - Padding.Horizontal;
        foreach (Control c in Controls)
        {
            int cw = w - c.Margin.Horizontal, h = RowHeight(c, cw);
            c.SetBounds(Padding.Left + c.Margin.Left, y + c.Margin.Top, c is Label { AutoSize: true } ? c.Width : cw, h);
            if (h > 0) y += h + c.Margin.Vertical;
        }
        int want = y + Padding.Bottom;
        if (Height != want) { Height = want; Parent?.PerformLayout(); }
        Invalidate();
    }

    public override Size GetPreferredSize(Size proposed) => new(proposed.Width > 0 ? proposed.Width : Width, HeightFor(proposed.Width > 0 ? proposed.Width : Width));
}

// Two things side by side: the left one takes what the right one leaves.
sealed class Split : Panel
{
    readonly Control left, right;
    public Split(Control left, Control right)
    {
        this.left = left; this.right = right;
        BackColor = Color.Transparent; Margin = new Padding(0);
        Controls.Add(left); Controls.Add(right);
    }
    int RightWidth => right.GetPreferredSize(Size.Empty).Width;
    int LeftHeight(int w) => left is VStack v ? v.HeightFor(w) : left.GetPreferredSize(new Size(w, 0)).Height;
    public int HeightFor(int width) => Math.Max(LeftHeight(width - RightWidth - right.Margin.Horizontal), right.GetPreferredSize(Size.Empty).Height + right.Margin.Vertical);
    protected override void OnLayout(LayoutEventArgs e)
    {
        var rs = right.GetPreferredSize(Size.Empty);
        int lw = Width - rs.Width - right.Margin.Horizontal;
        left.SetBounds(0, 0, lw, LeftHeight(lw));
        right.SetBounds(Width - rs.Width - right.Margin.Right, (Height - rs.Height) / 2, rs.Width, rs.Height);
    }
}

// A rounded panel with a title; rows are added with Add.
sealed class Card : VStack
{
    public Card(string? title = null, string? tag = null)
    {
        DoubleBuffered = true;
        ResizeRedraw = true;
        BackColor = Theme.Card;
        ForeColor = Theme.Text;
        Padding = new Padding(20, 16, 20, 18);
        Margin = new Padding(0, 0, 0, 14);
        if (title == null) return;
        var t = Theme.Text_(title, Theme.CardTitle); t.Margin = new Padding(0, 0, 0, 6);
        if (tag == null) { Add(t); return; }
        // a small amber label beside the title
        var g = Theme.Text_(tag, Theme.Small, Theme.Card);
        g.BackColor = Theme.Warn; g.Padding = new Padding(6, 1, 6, 2); g.Margin = new Padding(10, 4, 0, 6);
        var row = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false,
                                        Margin = new Padding(0), Padding = new Padding(0), BackColor = Color.Transparent };
        row.Controls.AddRange(new Control[] { t, g });
        Add(row);
    }
    protected override void OnPaintBackground(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Under(this));
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var p = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 10);
        using var b = new SolidBrush(Theme.Card);
        using var pen = new Pen(Theme.Border);
        e.Graphics.FillPath(b, p);
        e.Graphics.DrawPath(pen, p);
    }
}

// Flat buttons: Primary (accent) or secondary.
sealed class FlatBtn : Button
{
    bool primary;
    // Switches the accent look on and off after the button is made (the chosen one of a row of choices).
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Primary { get => primary; set { if (primary != value) { primary = value; Restyle(); Invalidate(); } } }
    public FlatBtn(string text, bool primary = false)
    {
        this.primary = primary;
        Text = text;
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        Font = primary ? Theme.Strong : Theme.Body;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(14, 5, 14, 5);
        Margin = new Padding(0, 4, 8, 0);
        Cursor = Cursors.Hand;
        UseVisualStyleBackColor = false;
        Restyle();
        EnabledChanged += (_, _) => Restyle();
    }
    void Restyle()
    {
        BackColor = !Enabled ? Theme.Raised : primary ? Theme.Accent : Theme.Raised;
        ForeColor = Enabled ? (primary ? Color.White : Theme.Text) : Theme.Off;
        FlatAppearance.MouseOverBackColor = primary ? Theme.AccentHover : Theme.RaisedHover;
        FlatAppearance.MouseDownBackColor = primary ? Theme.Accent : Theme.Raised;
    }
    // Disabled WinForms buttons draw grey text of their own; draw it ourselves so it stays readable on the dark card.
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        if (Enabled) return;
        using var b = new SolidBrush(BackColor);
        e.Graphics.FillRectangle(b, ClientRectangle);
        TextRenderer.DrawText(e.Graphics, Text, Font, ClientRectangle, ForeColor, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}

// An on/off switch.
sealed class Toggle : CheckBox
{
    public Toggle()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
        AutoSize = false;
        Size = new Size(46, 26);
        Margin = new Padding(0, 2, 14, 0);
        Cursor = Cursors.Hand;
        BackColor = Color.Transparent;
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Under(this));
        g.SmoothingMode = SmoothingMode.AntiAlias;
        float w = Width - 2, h = Math.Min(Height - 2, w * 0.56f);
        var r = new RectangleF(1, (Height - h) / 2, w, h);
        using var track = Theme.Round(r, h / 2);
        using var tb = new SolidBrush(!Enabled ? Theme.Raised : Checked ? Theme.Accent : Theme.Off);
        g.FillPath(tb, track);
        float k = h - 6;
        float x = Checked ? r.Right - k - 3 : r.X + 3;
        using var kb = new SolidBrush(Enabled ? Color.White : Theme.Muted);
        g.FillEllipse(kb, x, r.Y + 3, k, k);
        if (Focused && ShowFocusCues) { using var fp = new Pen(Theme.AccentHover); g.DrawPath(fp, track); }
    }
}

// The coloured dot in front of a status line.
sealed class Dot : Control
{
    Color color = Theme.Off;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public Color Color { get => color; set { if (color != value) { color = value; Invalidate(); } } }
    public Dot()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
        Size = new Size(12, 12);
        Margin = new Padding(0, 6, 8, 0);
        BackColor = Color.Transparent;
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Under(this));
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var b = new SolidBrush(color);
        e.Graphics.FillEllipse(b, 1, 1, Width - 3, Height - 3);
    }
}

// A dot and a line of text that can change.
sealed class StatusLine : FlowLayoutPanel
{
    public readonly Dot Dot = new();
    public readonly Label Label = Theme.Text_("", Theme.Strong);
    public StatusLine()
    {
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; WrapContents = false;
        Margin = new Padding(0, 0, 0, 2); Padding = new Padding(0); BackColor = Color.Transparent;
        Controls.Add(Dot); Controls.Add(Label);
    }
    public void Set(Color c, string text) { Dot.Color = c; if (Label.Text != text) Label.Text = text; }
}

// Left-side navigation entry.
sealed class NavButton : Control
{
    bool selected, hover;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public bool Selected { get => selected; set { selected = value; Invalidate(); } }
    public string Glyph { get; }
    public NavButton(string text, string glyph)
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.Selectable, true);
        Text = text; Glyph = glyph;
        Height = 40; Dock = DockStyle.Top; Cursor = Cursors.Hand;
        Font = Theme.Body;
        MouseEnter += (_, _) => { hover = true; Invalidate(); };
        MouseLeave += (_, _) => { hover = false; Invalidate(); };
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Side);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        if (selected || hover)
        {
            using var p = Theme.Round(new RectangleF(10, 3, Width - 20, Height - 6), 8);
            using var b = new SolidBrush(selected ? Theme.Card : Color.FromArgb(0x16, 0x19, 0x1f));
            g.FillPath(b, p);
        }
        if (selected) { using var a = new SolidBrush(Theme.Accent); using var bar = Theme.Round(new RectangleF(10, 12, 3, Height - 24), 1.5f); g.FillPath(a, bar); }
        var c = selected ? Theme.Text : Theme.Muted;
        using var gf = new Font("Segoe MDL2 Assets", 11f);
        TextRenderer.DrawText(g, Glyph, gf, new Rectangle(24, 0, 24, Height), c, TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
        TextRenderer.DrawText(g, Text, selected ? Theme.Strong : Font, new Rectangle(56, 0, Width - 60, Height), c, TextFormatFlags.VerticalCenter);
    }
    protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode is Keys.Enter or Keys.Space) OnClick(e); base.OnKeyDown(e); }
}

// A thin bar with a block sliding along it while something works.
sealed class BusyBar : Control
{
    readonly System.Windows.Forms.Timer t = new() { Interval = 16 };
    float pos;
    bool active;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Active { get => active; set { if (active == value) return; active = value; if (value) t.Start(); else t.Stop(); Parent?.PerformLayout(); } }
    public BusyBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        Height = 0; Margin = new Padding(0, 10, 0, 4);
        t.Tick += (_, _) => { pos = (pos + 0.012f) % 1.4f; Invalidate(); };
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Under(this));
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using var track = Theme.Round(new RectangleF(0, 0, Width - 1, Height - 1), (Height - 1) / 2f);
        using var tb = new SolidBrush(Theme.Raised);
        g.FillPath(tb, track);
        float bw = Width * 0.3f, x = (pos - 0.3f) * Width;
        var r = RectangleF.Intersect(new RectangleF(x, 0, bw, Height - 1), new RectangleF(0, 0, Width - 1, Height - 1));
        if (r.Width > 2) { using var bp = Theme.Round(r, (Height - 1) / 2f); using var ab = new SolidBrush(Theme.Accent); g.FillPath(ab, bp); }
    }
    protected override void Dispose(bool disposing) { if (disposing) t.Dispose(); base.Dispose(disposing); }
}

// A link-styled label.
sealed class LinkText : Label
{
    public LinkText(string text)
    {
        Text = text; AutoSize = true; Font = Theme.Small; ForeColor = Theme.AccentHover; Cursor = Cursors.Hand;
        Margin = new Padding(0, 2, 12, 2); BackColor = Color.Transparent;
        MouseEnter += (_, _) => Font = new Font(Theme.Small, FontStyle.Underline);
        MouseLeave += (_, _) => Font = Theme.Small;
    }
}
