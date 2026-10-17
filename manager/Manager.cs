//
// Bluetooth OBDX J2534 Manager - default Windows Forms theme, laid out like a
// Bosch-style VCI manager: a device explorer list with a big connection-status
// banner and Connect / Disconnect, plus Properties, Connection, Setup and Log
// tabs. One app: install/register, configure, connect/test, view the log.
//
// Built x86 (must match the 32-bit DLL it loads) against .NET Framework 4.8.1.
//
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8.1",
           FrameworkDisplayName = ".NET Framework 4.8.1")]

namespace BlueJ2534Manager
{
    public class MainForm : Form
    {
        const string RegSubKey = @"SOFTWARE\PassThruSupport.04.04\BlueJ2534";
        const string DeviceName = "Bluetooth OBDX J2534";
        const string Vendor = "Tester Present Specialist Automotive Solutions";
        const uint DEVICE_ID = 1;
        const uint READ_VBATT = 0x03;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)] static extern IntPtr LoadLibrary(string p);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string n);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool FreeLibrary(IntPtr m);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int DOpen(IntPtr n, out uint id);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int DClose(uint id);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int DReadVersion(uint id, StringBuilder a, StringBuilder b, StringBuilder c);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int DGetLastError(StringBuilder d);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int DIoctl(uint id, uint io, IntPtr i, IntPtr o);

        string m_dllDir;
        Image m_logo;
        Image m_btIcon;
        readonly List<PairedDevice> m_paired = new List<PairedDevice>();

        // live connection (persistent while "connected")
        IntPtr m_mod = IntPtr.Zero;
        uint m_dev;
        bool m_connected;
        DClose m_close;

        TabControl tabs;
        StatusStrip statusStrip;
        ToolStripStatusLabel statusLabel;

        // explorer
        Panel pnlStatus;
        Label lblStatusText, lblStatusDetail;
        int m_statusState;   // 0=disconnected, 1=connecting, 2=connected
        ListView lvDevice;
        Button btnConnect, btnDisconnect;
        CheckBox chkShowAll;
        // properties
        Label plFirmware, plDll, plApi, plVbatt, plLib, plReg;
        // connection
        RadioButton rbBt, rbCom;
        ComboBox cmbPaired, cmbPorts, cmbBaud;
        TextBox txtAddr, txtName;
        CheckBox chkLog;
        Label lblConnState;
        // setup
        TextBox txtDir;
        Label lblSetupState;
        // log
        TextBox txtLog;
        Label lblLogPath;
        CheckBox chkAuto;
        System.Windows.Forms.Timer logTimer;

        [STAThread]
        public static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }

        public MainForm()
        {
            Text = "Bluetooth OBDX J2534 Manager";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            ClientSize = new Size(470, 560);
            Font = new Font("Segoe UI", 9F);
            StartPosition = FormStartPosition.CenterScreen;
            try { m_logo = Image.FromStream(new MemoryStream(Convert.FromBase64String(Logo.Jpg))); } catch { m_logo = null; }
            m_btIcon = MakeBluetoothIcon(40);

            tabs = new TabControl();
            tabs.Dock = DockStyle.Fill; tabs.Padding = new Point(14, 5);
            Controls.Add(tabs);   // Fill added FIRST so the later Top/Bottom docks reserve their edges
            tabs.TabPages.Add(BuildExplorerTab());
            tabs.TabPages.Add(BuildPropertiesTab());
            tabs.TabPages.Add(BuildConnectionTab());
            tabs.TabPages.Add(BuildSetupTab());
            tabs.TabPages.Add(BuildLogTab());
            tabs.SelectedIndexChanged += delegate
            {
                if (tabs.SelectedIndex == 0) { RefreshDeviceList(); if (!m_connected) SetStatus(0, "Not connected", TargetDescr()); }
                if (tabs.SelectedIndex == 4) RefreshLog();
                logTimer.Enabled = tabs.SelectedIndex == 4 && chkAuto.Checked;
            };

            Controls.Add(BuildHeader());

            statusStrip = new StatusStrip();
            statusLabel = new ToolStripStatusLabel(); statusLabel.Spring = true; statusLabel.TextAlign = ContentAlignment.MiddleLeft;
            statusStrip.Items.Add(statusLabel);
            ToolStripStatusLabel flag = new ToolStripStatusLabel();
            flag.Image = MakeAustralianFlag(34, 17);
            flag.ImageScaling = ToolStripItemImageScaling.None;
            flag.Alignment = ToolStripItemAlignment.Right;
            flag.ToolTipText = "Made in Australia";
            statusStrip.Items.Add(flag);
            Controls.Add(statusStrip);

            FormClosing += delegate { if (m_connected) Disconnect(); };

            m_dllDir = FindInstallDir();
            LoadIni();
            LoadPairedDevices();
            LoadComPorts();
            OnModeChanged(null, EventArgs.Empty);
            RefreshDeviceList();
            RefreshStatus();
            SetStatus(0, "Not connected", TargetDescr());
        }

        // ---- header: title text (left) + Tester Present logo (right) ----
        Panel BuildHeader()
        {
            Panel h = new Panel();
            h.Dock = DockStyle.Top; h.Height = 62; h.BackColor = Color.White;

            Label t1 = new Label();
            t1.Text = "Tester Present Specialist Automotive Solutions";
            t1.Font = new Font("Segoe UI", 8.25F, FontStyle.Bold); t1.ForeColor = SystemColors.ControlText;
            t1.BackColor = Color.White; t1.SetBounds(12, 14, 262, 18);
            h.Controls.Add(t1);
            Label t2 = new Label();
            t2.Text = "Bluetooth PassThru for OBDX Pro";
            t2.Font = new Font("Segoe UI", 8F); t2.ForeColor = SystemColors.GrayText;
            t2.BackColor = Color.White; t2.SetBounds(12, 34, 262, 18);
            h.Controls.Add(t2);

            PictureBox pb = new PictureBox();
            pb.SetBounds(ClientSize.Width - 194, 3, 188, 56); pb.SizeMode = PictureBoxSizeMode.Zoom;
            pb.BackColor = Color.White;
            if (m_logo != null) pb.Image = m_logo;
            h.Controls.Add(pb);

            h.Paint += delegate (object s, PaintEventArgs e)
            {
                using (Pen pen = new Pen(SystemColors.ControlDark))
                    e.Graphics.DrawLine(pen, 0, h.Height - 1, h.Width, h.Height - 1);
            };
            return h;
        }

        // ---- Explorer tab (device list + engineering-grade status) ----
        TabPage BuildExplorerTab()
        {
            TabPage p = new TabPage("Explorer");

            // Status indicator: an LED + state text, framed - instrument-panel style.
            pnlStatus = new Panel();
            pnlStatus.SetBounds(12, 12, 434, 46); pnlStatus.BackColor = Color.White; pnlStatus.BorderStyle = BorderStyle.FixedSingle;
            pnlStatus.Paint += PaintStatusLed;
            lblStatusText = new Label(); lblStatusText.SetBounds(40, 7, 386, 20); lblStatusText.Font = new Font("Segoe UI", 10.5F, FontStyle.Bold);
            lblStatusText.BackColor = Color.White; lblStatusText.Text = "Not connected"; pnlStatus.Controls.Add(lblStatusText);
            lblStatusDetail = new Label(); lblStatusDetail.SetBounds(40, 27, 386, 15); lblStatusDetail.Font = new Font("Segoe UI", 8F);
            lblStatusDetail.ForeColor = SystemColors.GrayText; lblStatusDetail.BackColor = Color.White; pnlStatus.Controls.Add(lblStatusDetail);
            p.Controls.Add(pnlStatus);

            Button bd = new Button(); bd.Text = "Properties"; bd.SetBounds(12, 66, 90, 26); bd.Click += delegate { tabs.SelectedIndex = 1; }; p.Controls.Add(bd);
            Button bl = new Button(); bl.Text = "Open log"; bl.SetBounds(108, 66, 90, 26); bl.Click += delegate { tabs.SelectedIndex = 4; }; p.Controls.Add(bl);
            Button br = new Button(); br.Text = "Refresh"; br.SetBounds(356, 66, 90, 26); br.Click += delegate { RefreshDeviceList(); RefreshStatus(); }; p.Controls.Add(br);

            lvDevice = new ListView();
            lvDevice.SetBounds(12, 100, 434, 244); lvDevice.View = View.Tile; lvDevice.FullRowSelect = true;
            lvDevice.MultiSelect = false; lvDevice.BorderStyle = BorderStyle.Fixed3D; lvDevice.BackColor = Color.White;
            ImageList il = new ImageList(); il.ImageSize = new Size(40, 40); il.ColorDepth = ColorDepth.Depth32Bit;
            if (m_btIcon != null) il.Images.Add(m_btIcon);
            lvDevice.LargeImageList = il; lvDevice.TileSize = new Size(410, 46);
            p.Controls.Add(lvDevice);

            GroupBox g = new GroupBox();
            g.Text = "Connect / Disconnect"; g.SetBounds(12, 352, 434, 70);
            btnConnect = new Button(); btnConnect.Text = "Connect"; btnConnect.SetBounds(16, 28, 120, 30); btnConnect.Click += delegate { Connect(); }; g.Controls.Add(btnConnect);
            btnDisconnect = new Button(); btnDisconnect.Text = "Disconnect"; btnDisconnect.SetBounds(146, 28, 120, 30); btnDisconnect.Enabled = false; btnDisconnect.Click += delegate { Disconnect(); }; g.Controls.Add(btnDisconnect);
            Label hint = new Label(); hint.Text = "Reads firmware and\r\nbattery voltage."; hint.ForeColor = SystemColors.GrayText; hint.Font = new Font("Segoe UI", 8F); hint.SetBounds(282, 24, 146, 34); g.Controls.Add(hint);
            p.Controls.Add(g);
            return p;
        }

        // Instrument-panel status LED (red disconnected / amber connecting / green connected).
        void PaintStatusLed(object s, PaintEventArgs e)
        {
            Graphics g = e.Graphics; g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            Color c = m_statusState == 2 ? Color.FromArgb(0x2E, 0xA5, 0x3A)
                    : m_statusState == 1 ? Color.FromArgb(0xE2, 0x8C, 0x0E)
                    : Color.FromArgb(0xCE, 0x3A, 0x3A);
            int cy = pnlStatus.Height / 2;
            Rectangle led = new Rectangle(14, cy - 7, 14, 14);
            using (System.Drawing.Drawing2D.GraphicsPath gp = new System.Drawing.Drawing2D.GraphicsPath())
            {
                gp.AddEllipse(led);
                using (System.Drawing.Drawing2D.PathGradientBrush pg = new System.Drawing.Drawing2D.PathGradientBrush(gp))
                {
                    pg.CenterColor = ControlPaint.Light(c); pg.SurroundColors = new Color[] { c };
                    pg.CenterPoint = new PointF(led.X + 5, led.Y + 4);
                    g.FillEllipse(pg, led);
                }
            }
            using (Pen pen = new Pen(Color.FromArgb(70, 0, 0, 0))) g.DrawEllipse(pen, led);
        }

        // Build the official Bluetooth logo (white rune on a blue rounded square).
        Image MakeBluetoothIcon(int size)
        {
            Bitmap bmp = new Bitmap(size, size);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);
                Color blue = Color.FromArgb(0x00, 0x82, 0xFC);     // Bluetooth brand blue
                Rectangle r = new Rectangle(1, 1, size - 2, size - 2);
                using (System.Drawing.Drawing2D.GraphicsPath gp = RoundRect(r, size / 5))
                using (SolidBrush b = new SolidBrush(blue)) g.FillPath(b, gp);
                DrawBtRune(g, new RectangleF(size * 0.37f, size * 0.20f, size * 0.26f, size * 0.60f),
                           Color.White, Math.Max(1.6f, size * 0.07f));
            }
            return bmp;
        }
        static System.Drawing.Drawing2D.GraphicsPath RoundRect(Rectangle r, int rad)
        {
            System.Drawing.Drawing2D.GraphicsPath p = new System.Drawing.Drawing2D.GraphicsPath();
            int d = rad * 2;
            p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure(); return p;
        }
        // Programmatically drawn Australian flag (Union Jack canton,
        // Commonwealth Star, Southern Cross). Ratio 1:2.
        Image MakeAustralianFlag(int w, int h)
        {
            Bitmap bmp = new Bitmap(w, h);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                Color blue = Color.FromArgb(0, 36, 125);
                Color red = Color.FromArgb(0xCF, 0x14, 0x2B);
                using (SolidBrush b = new SolidBrush(blue)) g.FillRectangle(b, 0, 0, w, h);

                DrawUnionJack(g, new RectangleF(0, 0, w / 2f, h / 2f), blue, red);

                // Commonwealth Star (7-pointed) under the canton.
                FillStar(g, new PointF(w / 4f, h * 0.73f), h * 0.17f, h * 0.075f, 7, Color.White);

                // Southern Cross on the fly half (approximate positions).
                float sr = h * 0.095f, si = sr * 0.45f;
                FillStar(g, new PointF(w * 0.74f, h * 0.24f), sr, si, 7, Color.White); // Gamma
                FillStar(g, new PointF(w * 0.62f, h * 0.50f), sr, si, 7, Color.White); // Beta
                FillStar(g, new PointF(w * 0.86f, h * 0.46f), sr, si, 7, Color.White); // Delta
                FillStar(g, new PointF(w * 0.74f, h * 0.80f), sr, si, 7, Color.White); // Alpha
                FillStar(g, new PointF(w * 0.80f, h * 0.60f), sr * 0.7f, si * 0.7f, 5, Color.White); // Epsilon
            }
            return bmp;
        }
        static void DrawUnionJack(Graphics g, RectangleF r, Color blue, Color red)
        {
            System.Drawing.Region old = g.Clip;
            g.SetClip(r);
            using (Pen pw = new Pen(Color.White, r.Height * 0.32f))
            { g.DrawLine(pw, r.Left, r.Top, r.Right, r.Bottom); g.DrawLine(pw, r.Right, r.Top, r.Left, r.Bottom); }
            using (Pen pr = new Pen(red, r.Height * 0.13f))
            { g.DrawLine(pr, r.Left, r.Top, r.Right, r.Bottom); g.DrawLine(pr, r.Right, r.Top, r.Left, r.Bottom); }
            float cw = r.Height * 0.36f;
            using (SolidBrush w = new SolidBrush(Color.White))
            { g.FillRectangle(w, r.X + r.Width / 2f - cw / 2, r.Y, cw, r.Height); g.FillRectangle(w, r.X, r.Y + r.Height / 2f - cw / 2, r.Width, cw); }
            float rw = r.Height * 0.19f;
            using (SolidBrush rb = new SolidBrush(red))
            { g.FillRectangle(rb, r.X + r.Width / 2f - rw / 2, r.Y, rw, r.Height); g.FillRectangle(rb, r.X, r.Y + r.Height / 2f - rw / 2, r.Width, rw); }
            g.Clip = old;
        }
        static void FillStar(Graphics g, PointF c, float rOut, float rIn, int pts, Color col)
        {
            PointF[] p = new PointF[pts * 2];
            for (int i = 0; i < pts * 2; i++)
            {
                double ang = -Math.PI / 2 + i * Math.PI / pts;
                float r = (i % 2 == 0) ? rOut : rIn;
                p[i] = new PointF(c.X + (float)(Math.Cos(ang) * r), c.Y + (float)(Math.Sin(ang) * r));
            }
            using (SolidBrush b = new SolidBrush(col)) g.FillPolygon(b, p);
        }

        static void DrawBtRune(Graphics g, RectangleF b, Color c, float thick)
        {
            float x0 = b.Left, x1 = b.Right, cx = b.Left + b.Width / 2f;
            float y0 = b.Top, y4 = b.Bottom, y1 = b.Top + b.Height * 0.25f, y3 = b.Top + b.Height * 0.75f;
            PointF[] pts = { new PointF(x0, y1), new PointF(x1, y3), new PointF(cx, y4),
                             new PointF(cx, y0), new PointF(x1, y1), new PointF(x0, y3) };
            using (Pen pen = new Pen(c, thick))
            {
                pen.StartCap = System.Drawing.Drawing2D.LineCap.Round; pen.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                pen.LineJoin = System.Drawing.Drawing2D.LineJoin.Round;
                g.DrawLines(pen, pts);
            }
        }

        // ---- Properties tab ----
        TabPage BuildPropertiesTab()
        {
            TabPage p = new TabPage("Properties");
            GroupBox g = new GroupBox(); g.Text = "Device"; g.SetBounds(12, 12, 434, 200);
            plReg = PropRow(g, "Registration", 0);
            plFirmware = PropRow(g, "Firmware", 1);
            plApi = PropRow(g, "J2534 API", 2);
            plDll = PropRow(g, "DLL version", 3);
            plVbatt = PropRow(g, "Battery", 4);
            plLib = PropRow(g, "Library path", 5);
            p.Controls.Add(g);
            Label note = new Label(); note.Text = "Values populate when you connect on the Explorer tab.";
            note.ForeColor = SystemColors.GrayText; note.SetBounds(14, 220, 420, 18); p.Controls.Add(note);
            return p;
        }
        Label PropRow(GroupBox g, string caption, int row)
        {
            int y = 28 + row * 27;
            Label c = new Label(); c.Text = caption + ":"; c.SetBounds(14, y, 100, 18); g.Controls.Add(c);
            Label v = new Label(); v.Text = "-"; v.SetBounds(118, y, 304, 18); v.AutoEllipsis = true; v.Font = new Font("Segoe UI", 9F, FontStyle.Bold); g.Controls.Add(v);
            return v;
        }

        // ---- Connection tab ----
        TabPage BuildConnectionTab()
        {
            TabPage p = new TabPage("Connection");
            GroupBox g = new GroupBox(); g.Text = "Transport"; g.SetBounds(12, 12, 434, 240);
            rbBt = new RadioButton(); rbBt.Text = "Bluetooth direct (RFCOMM / SPP)"; rbBt.SetBounds(14, 24, 260, 20); rbBt.Checked = true; rbBt.CheckedChanged += OnModeChanged; g.Controls.Add(rbBt);
            Label lp = new Label(); lp.Text = "Paired device:"; lp.SetBounds(34, 52, 86, 18); g.Controls.Add(lp);
            cmbPaired = new ComboBox(); cmbPaired.DropDownStyle = ComboBoxStyle.DropDownList; cmbPaired.SetBounds(122, 49, 230, 23); cmbPaired.SelectedIndexChanged += OnPairedSelected; g.Controls.Add(cmbPaired);
            Button rsc = new Button(); rsc.Text = "Rescan"; rsc.SetBounds(358, 48, 62, 25); rsc.Click += delegate { LoadPairedDevices(); }; g.Controls.Add(rsc);
            chkShowAll = new CheckBox(); chkShowAll.Text = "Show all paired Bluetooth devices (not just OBD adapters)"; chkShowAll.SetBounds(36, 76, 380, 18);
            chkShowAll.Font = new Font("Segoe UI", 8F); chkShowAll.CheckedChanged += delegate { ApplyPairedFilter(); }; g.Controls.Add(chkShowAll);
            Label la = new Label(); la.Text = "Address:"; la.SetBounds(34, 104, 86, 18); g.Controls.Add(la);
            txtAddr = new TextBox(); txtAddr.SetBounds(122, 101, 150, 23); g.Controls.Add(txtAddr);
            Label lah = new Label(); lah.Text = "(blank = by name)"; lah.ForeColor = SystemColors.GrayText; lah.SetBounds(280, 104, 140, 18); g.Controls.Add(lah);
            Label ln = new Label(); ln.Text = "Name filter:"; ln.SetBounds(34, 132, 86, 18); g.Controls.Add(ln);
            txtName = new TextBox(); txtName.SetBounds(122, 129, 150, 23); g.Controls.Add(txtName);
            rbCom = new RadioButton(); rbCom.Text = "Bluetooth virtual COM port:"; rbCom.SetBounds(14, 166, 190, 20); rbCom.CheckedChanged += OnModeChanged; g.Controls.Add(rbCom);
            cmbPorts = new ComboBox(); cmbPorts.DropDownStyle = ComboBoxStyle.DropDown; cmbPorts.SetBounds(206, 164, 90, 23); g.Controls.Add(cmbPorts);
            Label lb = new Label(); lb.Text = "Baud:"; lb.SetBounds(306, 167, 40, 18); g.Controls.Add(lb);
            cmbBaud = new ComboBox(); cmbBaud.DropDownStyle = ComboBoxStyle.DropDownList; cmbBaud.Items.AddRange(new object[] { "38400", "115200", "500000", "1000000" }); cmbBaud.SelectedIndex = 0; cmbBaud.SetBounds(346, 164, 74, 23); g.Controls.Add(cmbBaud);
            chkLog = new CheckBox(); chkLog.Text = "Enable debug log (BlueJ2534.log)"; chkLog.Checked = true; chkLog.SetBounds(14, 198, 280, 20); g.Controls.Add(chkLog);
            p.Controls.Add(g);
            Button save = new Button(); save.Text = "Save settings"; save.SetBounds(12, 260, 120, 28); save.Click += OnSave; p.Controls.Add(save);
            lblConnState = new Label(); lblConnState.SetBounds(140, 266, 306, 18); lblConnState.AutoEllipsis = true; p.Controls.Add(lblConnState);
            return p;
        }

        // ---- Setup tab ----
        TabPage BuildSetupTab()
        {
            TabPage p = new TabPage("Setup");
            GroupBox g = new GroupBox(); g.Text = "Install location"; g.SetBounds(12, 12, 434, 66);
            txtDir = new TextBox(); txtDir.SetBounds(14, 28, 320, 23);
            txtDir.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "BlueJ2534");
            g.Controls.Add(txtDir);
            Button br = new Button(); br.Text = "Browse..."; br.SetBounds(342, 27, 80, 25); br.Click += OnBrowse; g.Controls.Add(br);
            p.Controls.Add(g);
            GroupBox a = new GroupBox(); a.Text = "Register device"; a.SetBounds(12, 86, 434, 128);
            Label note = new Label(); note.Text = "Install copies BlueJ2534.dll, writes BlueJ2534.ini from the Connection\r\ntab, and registers the device for all 32-bit J2534 applications.";
            note.SetBounds(14, 24, 410, 36); a.Controls.Add(note);
            Button bi = new Button(); bi.Text = "Install / Update"; bi.SetBounds(14, 66, 120, 30); bi.Click += OnInstall; a.Controls.Add(bi);
            Button bu = new Button(); bu.Text = "Uninstall"; bu.SetBounds(142, 66, 100, 30); bu.Click += OnUninstall; a.Controls.Add(bu);
            lblSetupState = new Label(); lblSetupState.SetBounds(14, 102, 410, 18); lblSetupState.AutoEllipsis = true; a.Controls.Add(lblSetupState);
            p.Controls.Add(a);
            return p;
        }

        // ---- Log tab ----
        TabPage BuildLogTab()
        {
            TabPage p = new TabPage("Log");
            Button rf = new Button(); rf.Text = "Refresh"; rf.SetBounds(12, 12, 78, 26); rf.Click += delegate { RefreshLog(); }; p.Controls.Add(rf);
            chkAuto = new CheckBox(); chkAuto.Text = "Auto"; chkAuto.SetBounds(98, 15, 56, 20); chkAuto.CheckedChanged += delegate { logTimer.Enabled = chkAuto.Checked && tabs.SelectedIndex == 4; }; p.Controls.Add(chkAuto);
            Button cl = new Button(); cl.Text = "Clear"; cl.SetBounds(160, 12, 78, 26); cl.Click += delegate { ClearLog(); }; p.Controls.Add(cl);
            Button of = new Button(); of.Text = "Open folder"; of.SetBounds(244, 12, 96, 26); of.Click += delegate { OpenLogFolder(); }; p.Controls.Add(of);
            lblLogPath = new Label(); lblLogPath.ForeColor = SystemColors.GrayText; lblLogPath.SetBounds(12, 42, 434, 16); lblLogPath.AutoEllipsis = true; p.Controls.Add(lblLogPath);
            txtLog = new TextBox(); txtLog.Multiline = true; txtLog.ReadOnly = true; txtLog.ScrollBars = ScrollBars.Both; txtLog.WordWrap = false;
            txtLog.Font = new Font("Consolas", 8.75F); txtLog.BackColor = Color.White; txtLog.SetBounds(12, 62, 434, 364); p.Controls.Add(txtLog);
            logTimer = new System.Windows.Forms.Timer(); logTimer.Interval = 1000; logTimer.Tick += delegate { if (tabs.SelectedIndex == 4) RefreshLog(); };
            return p;
        }

        public void PreviewPage(int i) { if (tabs != null && i >= 0 && i < tabs.TabCount) tabs.SelectedIndex = i; }

        // ================================================================
        // Device list / banner
        // ================================================================
        void SetStatus(int state, string text, string detail)
        {
            m_statusState = state;
            if (lblStatusText != null) lblStatusText.Text = text;
            if (lblStatusDetail != null) lblStatusDetail.Text = detail != null ? detail : "";
            if (pnlStatus != null) pnlStatus.Invalidate();
        }
        string TargetDescr()
        {
            if (rbCom != null && rbCom.Checked) return cmbPorts.Text.Trim().Length > 0 ? "Virtual COM port " + cmbPorts.Text.Trim() : "";
            string a = txtAddr != null ? txtAddr.Text.Trim() : "";
            if (a.Length > 0) return "Bluetooth SPP   " + a;
            string n = txtName != null ? txtName.Text.Trim() : "";
            return n.Length > 0 ? "Bluetooth SPP   discover \"" + n + "\"" : "No transport configured";
        }

        void RefreshDeviceList()
        {
            if (lvDevice == null) return;
            lvDevice.Items.Clear();
            string addr = txtAddr != null ? txtAddr.Text.Trim() : "";
            string where = (rbCom != null && rbCom.Checked) ? (cmbPorts.Text.Trim()) : (addr.Length > 0 ? addr : (txtName != null ? txtName.Text.Trim() : ""));
            ListViewItem it = new ListViewItem(DeviceName);
            if (m_logo != null) it.ImageIndex = 0;
            it.SubItems.Add(where.Length > 0 ? where : "(not configured)");
            lvDevice.Items.Add(it);
            lvDevice.Items[0].Selected = true;
        }

        void RefreshStatus()
        {
            string dll = RegisteredDll();
            string s = dll != null ? "Registered" : "Not registered";
            if (plReg != null) { plReg.Text = dll != null ? "Registered (32-bit)" : "Not registered"; plReg.ForeColor = dll != null ? Color.Green : Color.Firebrick; }
            if (plLib != null) plLib.Text = dll != null ? dll : "-";
            if (statusLabel != null) statusLabel.Text = s + "   -   " + (dll != null ? dll : "use the Setup tab to register");
        }

        // ================================================================
        // Connect / Disconnect (persistent session)
        // ================================================================
        void Connect()
        {
            if (m_connected) return;
            string dll = RegisteredDll();
            if (dll == null || !File.Exists(dll)) dll = Path.Combine(m_dllDir, "BlueJ2534.dll");
            if (!File.Exists(dll)) { SetStatus(0, "DLL not found", "Register the device on the Setup tab"); return; }

            btnConnect.Enabled = false;
            SetStatus(1, "Connecting...", TargetDescr());
            string path = dll;
            ThreadPool.QueueUserWorkItem(delegate { DoConnect(path); });
        }

        void DoConnect(string dllPath)
        {
            IntPtr mod = LoadLibrary(dllPath);
            if (mod == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                UI(delegate { SetStatus(0, "Load failed", "LoadLibrary Win32 " + err); btnConnect.Enabled = true;
                    MessageBox.Show(this, "LoadLibrary failed, Win32 " + err + (err == 193 ? " (bitness mismatch)" : ""), Text); });
                return;
            }
            DOpen open = Del<DOpen>(mod, "PassThruOpen");
            DReadVersion rv = Del<DReadVersion>(mod, "PassThruReadVersion");
            DGetLastError gle = Del<DGetLastError>(mod, "PassThruGetLastError");
            DIoctl ioctl = Del<DIoctl>(mod, "PassThruIoctl");
            DClose close = Del<DClose>(mod, "PassThruClose");
            if (open == null || close == null) { FreeLibrary(mod); UI(delegate { SetStatus(0, "Bad DLL", "Required exports missing"); btnConnect.Enabled = true; }); return; }

            uint dev;
            int r = open(IntPtr.Zero, out dev);
            if (r != 0)
            {
                StringBuilder eb = new StringBuilder(128); if (gle != null) gle(eb);
                string msg = eb.ToString();
                FreeLibrary(mod);
                UI(delegate { SetStatus(0, "Not connected", "PassThruOpen failed (" + r + ")"); btnConnect.Enabled = true;
                    MessageBox.Show(this, "PassThruOpen failed (" + r + ")\r\n" + msg, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); });
                return;
            }

            string fwStr = "", dlStr = "", apiStr = "", vbStr = "-";
            if (rv != null)
            {
                StringBuilder fw = new StringBuilder(128), dl = new StringBuilder(128), api = new StringBuilder(128);
                if (rv(dev, fw, dl, api) == 0) { fwStr = fw.ToString(); dlStr = dl.ToString(); apiStr = api.ToString(); }
            }
            if (ioctl != null)
            {
                IntPtr op = Marshal.AllocHGlobal(4);
                try { Marshal.WriteInt32(op, 0); if (ioctl(DEVICE_ID, READ_VBATT, IntPtr.Zero, op) == 0) {
                    uint mv = (uint)Marshal.ReadInt32(op); vbStr = (mv / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) + " V"; } }
                finally { Marshal.FreeHGlobal(op); }
            }

            m_mod = mod; m_dev = dev; m_close = close; m_connected = true;
            string fFw = fwStr, fDl = dlStr, fApi = apiStr, fVb = vbStr;
            UI(delegate {
                string det = (fFw.Length > 0 ? fFw : "") + (fVb != "-" && fVb.Length > 0 ? "    battery " + fVb : "");
                SetStatus(2, "Connected", det.Trim().Length > 0 ? det : TargetDescr());
                btnConnect.Enabled = false; btnDisconnect.Enabled = true;
                if (plFirmware != null) plFirmware.Text = fFw.Length > 0 ? fFw : "(n/a)";
                if (plDll != null) plDll.Text = fDl;
                if (plApi != null) plApi.Text = fApi;
                if (plVbatt != null) plVbatt.Text = fVb;
            });
        }

        void Disconnect()
        {
            try { if (m_connected && m_close != null) m_close(m_dev); } catch { }
            if (m_mod != IntPtr.Zero) { FreeLibrary(m_mod); m_mod = IntPtr.Zero; }
            m_connected = false; m_close = null;
            if (btnConnect != null) { btnConnect.Enabled = true; btnDisconnect.Enabled = false; SetStatus(0, "Not connected", TargetDescr()); }
            if (plFirmware != null) { plFirmware.Text = "-"; plDll.Text = "-"; plApi.Text = "-"; plVbatt.Text = "-"; }
        }

        void UI(MethodInvoker a) { if (InvokeRequired) BeginInvoke(a); else a(); }
        static T Del<T>(IntPtr mod, string name) where T : class
        {
            IntPtr p = GetProcAddress(mod, name);
            if (p == IntPtr.Zero) return null;
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        // ================================================================
        // Registry / paths
        // ================================================================
        RegistryKey OpenReg(bool writable)
        {
            using (RegistryKey hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                return writable ? hklm.CreateSubKey(RegSubKey) : hklm.OpenSubKey(RegSubKey);
        }
        string RegisteredDll()
        {
            try { using (RegistryKey k = OpenReg(false)) { if (k != null) return k.GetValue("FunctionLibrary") as string; } } catch { }
            return null;
        }
        string FindInstallDir()
        {
            string dll = RegisteredDll();
            if (dll != null && File.Exists(dll)) return Path.GetDirectoryName(dll);
            string exeDir = Path.GetDirectoryName(Application.ExecutablePath);
            if (File.Exists(Path.Combine(exeDir, "BlueJ2534.dll"))) return exeDir;
            string bin = Path.Combine(exeDir, "bin");
            if (File.Exists(Path.Combine(bin, "BlueJ2534.dll"))) return bin;
            return exeDir;
        }
        string IniPath() { return Path.Combine(m_dllDir, "BlueJ2534.ini"); }
        string LogPath()
        {
            string inDir = Path.Combine(m_dllDir, "BlueJ2534.log");
            if (File.Exists(inDir)) return inDir;
            string local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BlueJ2534", "BlueJ2534.log");
            if (File.Exists(local)) return local;
            return inDir;
        }

        // ================================================================
        // INI
        // ================================================================
        void LoadIni()
        {
            try
            {
                string ini = IniPath();
                if (!File.Exists(ini)) { lblConnState.Text = "No BlueJ2534.ini yet - defaults shown."; return; }
                foreach (string raw in File.ReadAllLines(ini))
                {
                    string line = raw.Trim(); int eq = line.IndexOf('=');
                    if (eq <= 0 || line.StartsWith(";") || line.StartsWith("[")) continue;
                    string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
                    if (k.Equals("Mode", StringComparison.OrdinalIgnoreCase)) { rbCom.Checked = v.Equals("com", StringComparison.OrdinalIgnoreCase); rbBt.Checked = !rbCom.Checked; }
                    else if (k.Equals("BtAddress", StringComparison.OrdinalIgnoreCase)) txtAddr.Text = v;
                    else if (k.Equals("BtName", StringComparison.OrdinalIgnoreCase)) txtName.Text = v;
                    else if (k.Equals("ComPort", StringComparison.OrdinalIgnoreCase)) cmbPorts.Text = v;
                    else if (k.Equals("Baud", StringComparison.OrdinalIgnoreCase)) cmbBaud.Text = v;
                    else if (k.Equals("Log", StringComparison.OrdinalIgnoreCase)) chkLog.Checked = v != "0";
                }
                lblConnState.Text = "Loaded " + ini;
            }
            catch (Exception ex) { lblConnState.ForeColor = Color.Firebrick; lblConnState.Text = "Load error: " + ex.Message; }
        }
        string BuildIni()
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("; BlueJ2534 configuration (written by BlueJ2534 Manager)");
            sb.AppendLine("[connection]");
            sb.AppendLine("Mode=" + (rbBt.Checked ? "bt" : "com"));
            sb.AppendLine("BtAddress=" + (rbBt.Checked ? txtAddr.Text.Trim() : ""));
            sb.AppendLine("BtName=" + txtName.Text.Trim());
            sb.AppendLine("ComPort=" + cmbPorts.Text.Trim());
            sb.AppendLine("Baud=" + (cmbBaud.Text.Trim().Length > 0 ? cmbBaud.Text.Trim() : "38400"));
            sb.AppendLine("[options]");
            sb.AppendLine("Log=" + (chkLog.Checked ? "1" : "0"));
            return sb.ToString();
        }
        bool ValidateConn()
        {
            if (rbBt.Checked && txtAddr.Text.Trim().Length == 0 && txtName.Text.Trim().Length == 0)
            { MessageBox.Show(this, "Bluetooth mode needs an address or a name filter.", Text); return false; }
            if (rbCom.Checked && cmbPorts.Text.Trim().Length == 0)
            { MessageBox.Show(this, "COM mode needs a port.", Text); return false; }
            return true;
        }
        void OnSave(object s, EventArgs e)
        {
            if (!ValidateConn()) return;
            try { File.WriteAllText(IniPath(), BuildIni()); lblConnState.ForeColor = Color.Green; lblConnState.Text = "Saved " + IniPath(); RefreshDeviceList(); }
            catch (Exception ex) { lblConnState.ForeColor = Color.Firebrick; lblConnState.Text = "Save failed: " + ex.Message; }
        }

        // ================================================================
        // Install / Uninstall
        // ================================================================
        void OnBrowse(object s, EventArgs e)
        {
            using (FolderBrowserDialog dlg = new FolderBrowserDialog())
            {
                dlg.Description = "Choose the install folder";
                if (Directory.Exists(txtDir.Text)) dlg.SelectedPath = txtDir.Text;
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    string pth = dlg.SelectedPath;
                    if (!string.Equals(Path.GetFileName(pth.TrimEnd('\\')), "BlueJ2534", StringComparison.OrdinalIgnoreCase))
                        pth = Path.Combine(pth, "BlueJ2534");
                    txtDir.Text = pth;
                }
            }
        }
        string FindSourceDll()
        {
            string exeDir = Path.GetDirectoryName(Application.ExecutablePath);
            string[] cands = { Path.Combine(exeDir, "BlueJ2534.dll"), Path.Combine(exeDir, "bin", "BlueJ2534.dll"),
                               Path.Combine(Path.GetDirectoryName(exeDir) ?? exeDir, "bin", "BlueJ2534.dll") };
            foreach (string c in cands) if (File.Exists(c)) return c;
            return null;
        }
        void SetupMsg(string m, bool err) { lblSetupState.ForeColor = err ? Color.Firebrick : Color.Green; lblSetupState.Text = m; }
        void OnInstall(object s, EventArgs e)
        {
            if (!ValidateConn()) return;
            if (m_connected) Disconnect();
            try
            {
                string dir = txtDir.Text.Trim();
                if (dir.Length == 0) { SetupMsg("Choose a folder.", true); return; }
                string src = FindSourceDll();
                if (src == null) { MessageBox.Show(this, "BlueJ2534.dll not found next to the app.", Text, MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
                Directory.CreateDirectory(dir);
                string dst = Path.Combine(dir, "BlueJ2534.dll");
                File.Copy(src, dst, true);
                File.WriteAllText(Path.Combine(dir, "BlueJ2534.ini"), BuildIni());
                using (RegistryKey k = OpenReg(true))
                {
                    k.SetValue("Name", DeviceName); k.SetValue("Vendor", Vendor);
                    k.SetValue("FunctionLibrary", dst); k.SetValue("ConfigApplication", Application.ExecutablePath);
                    k.SetValue("CAN", 1, RegistryValueKind.DWord); k.SetValue("ISO15765", 1, RegistryValueKind.DWord);
                    k.SetValue("CAN_PS", 1, RegistryValueKind.DWord); k.SetValue("ISO15765_PS", 1, RegistryValueKind.DWord);
                    k.SetValue("J1850VPW", 0, RegistryValueKind.DWord); k.SetValue("ISO9141", 0, RegistryValueKind.DWord);
                    k.SetValue("ISO14230", 0, RegistryValueKind.DWord); k.SetValue("J1850PWM", 0, RegistryValueKind.DWord);
                }
                m_dllDir = dir; SetupMsg("Installed and registered.", false); RefreshStatus(); RefreshDeviceList();
            }
            catch (Exception ex) { SetupMsg("Install failed: " + ex.Message, true); MessageBox.Show(this, ex.Message, "Install failed", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
        void OnUninstall(object s, EventArgs e)
        {
            if (m_connected) Disconnect();
            try
            {
                string dll = RegisteredDll();
                if (dll == null) { SetupMsg("Nothing to uninstall.", false); RefreshStatus(); return; }
                if (MessageBox.Show(this, "Remove the BlueJ2534 device registration?\r\n\r\n" + dll, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                using (RegistryKey hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                    hklm.DeleteSubKeyTree(RegSubKey, false);
                SetupMsg("Registration removed.", false);
                string dir = Path.GetDirectoryName(dll);
                if (Directory.Exists(dir) && MessageBox.Show(this, "Also delete the install folder?\r\n" + dir, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                { try { Directory.Delete(dir, true); } catch { } }
                RefreshStatus();
            }
            catch (Exception ex) { SetupMsg("Uninstall failed: " + ex.Message, true); }
        }

        // ================================================================
        // Paired devices / COM ports
        // ================================================================
        class PairedDevice { public string Name; public string Address; public override string ToString() { return Name + "  [" + Address + "]"; } }

        static readonly string[] ObdKeywords = { "OBDX", "OBD", "ELM", "VGATE", "VLINK", "VEEPEAK", "OBDLINK", "KONNWEI", "ICAR", "VINT", "SCANTOOL", "KINGBOLEN" };

        // Read every paired classic-BT device from the Windows registry (these
        // are the machine's real paired devices, not a fixed list).
        void LoadPairedDevices()
        {
            m_paired.Clear();
            try
            {
                using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices"))
                {
                    if (k != null)
                        foreach (string mac in k.GetSubKeyNames())
                        {
                            if (mac.Length != 12) continue;
                            string name = "(unknown)";
                            using (RegistryKey dk = k.OpenSubKey(mac))
                            {
                                object v = dk == null ? null : dk.GetValue("Name");
                                byte[] raw = v as byte[];
                                if (raw != null) { try { name = Encoding.UTF8.GetString(raw).TrimEnd('\0'); } catch { name = Encoding.ASCII.GetString(raw).TrimEnd('\0'); } }
                            }
                            StringBuilder mb = new StringBuilder();
                            for (int i = 0; i < 12; i += 2) { if (i > 0) mb.Append(':'); mb.Append(mac.Substring(i, 2).ToUpperInvariant()); }
                            PairedDevice d = new PairedDevice(); d.Name = name; d.Address = mb.ToString(); m_paired.Add(d);
                        }
                }
            }
            catch { }
            ApplyPairedFilter();
        }

        // Show only likely OBD adapters by default (an OBD tool shouldn't list
        // the shop's phones and speakers); "Show all" reveals everything.
        void ApplyPairedFilter()
        {
            if (cmbPaired == null) return;
            cmbPaired.Items.Clear();
            bool all = chkShowAll != null && chkShowAll.Checked;
            string cfg = txtAddr != null ? txtAddr.Text.Trim().ToUpperInvariant() : "";
            List<PairedDevice> shown = new List<PairedDevice>();
            foreach (PairedDevice d in m_paired)
            {
                if (all) { shown.Add(d); continue; }
                string un = d.Name.ToUpperInvariant();
                bool obd = false;
                foreach (string kw in ObdKeywords) if (un.IndexOf(kw, StringComparison.Ordinal) >= 0) { obd = true; break; }
                if (obd || (cfg.Length > 0 && d.Address.ToUpperInvariant() == cfg)) shown.Add(d);
            }
            if (shown.Count == 0) shown.AddRange(m_paired);   // nothing matched -> don't hide everything
            foreach (PairedDevice d in shown) cmbPaired.Items.Add(d);

            for (int i = 0; i < cmbPaired.Items.Count; i++)
            {
                string n = ((PairedDevice)cmbPaired.Items[i]).Name.ToUpperInvariant();
                if (n.Contains("OBDX") || n.Contains("OBD") || n.Contains("ELM")) { cmbPaired.SelectedIndex = i; break; }
            }
            if (cmbPaired.SelectedIndex < 0 && cmbPaired.Items.Count > 0) cmbPaired.SelectedIndex = 0;
        }
        void OnPairedSelected(object s, EventArgs e)
        {
            PairedDevice d = cmbPaired.SelectedItem as PairedDevice;
            if (d != null) { txtAddr.Text = d.Address; if (txtName.Text.Trim().Length == 0 && d.Name != "(unknown)") txtName.Text = d.Name; RefreshDeviceList(); }
        }
        void LoadComPorts()
        {
            cmbPorts.Items.Clear();
            try { foreach (string pn in System.IO.Ports.SerialPort.GetPortNames()) cmbPorts.Items.Add(pn); } catch { }
        }
        void OnModeChanged(object s, EventArgs e)
        {
            bool bt = rbBt.Checked;
            cmbPaired.Enabled = bt; txtAddr.Enabled = bt; txtName.Enabled = bt; cmbPorts.Enabled = !bt;
        }

        // ================================================================
        // Log viewer
        // ================================================================
        void RefreshLog()
        {
            string path = LogPath(); lblLogPath.Text = path;
            try
            {
                if (!File.Exists(path)) { txtLog.Text = "(no log file yet)"; return; }
                string text;
                using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (StreamReader sr = new StreamReader(fs))
                { if (fs.Length > 65536) fs.Seek(-65536, SeekOrigin.End); text = sr.ReadToEnd(); }
                bool bottom = txtLog.SelectionStart >= txtLog.TextLength - 2;
                txtLog.Text = text;
                if (bottom) { txtLog.SelectionStart = txtLog.TextLength; txtLog.ScrollToCaret(); }
            }
            catch (Exception ex) { txtLog.Text = "Cannot read log: " + ex.Message; }
        }
        void ClearLog() { try { string p = LogPath(); if (File.Exists(p)) File.WriteAllText(p, ""); RefreshLog(); } catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); } }
        void OpenLogFolder() { try { string d = Path.GetDirectoryName(LogPath()); if (Directory.Exists(d)) System.Diagnostics.Process.Start(d); } catch { } }
    }
}
