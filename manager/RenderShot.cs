// Offscreen render harness: instantiates the real MainForm and saves its
// rendered client area to a PNG, so the actual UI can be previewed without a
// live screen capture. Compiled with /main:BlueJ2534Manager.RenderShot.
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Windows.Forms;

namespace BlueJ2534Manager
{
    internal static class RenderShot
    {
        [STAThread]
        public static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(true);
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
            string outPath = args.Length > 0 ? args[0] : "manager_preview.png";
            int page = args.Length > 1 ? int.Parse(args[1]) : 0;
            try {
            using (MainForm f = new MainForm())
            {
                f.StartPosition = FormStartPosition.Manual;
                f.Location = new Point(-4000, -4000);
                f.Show();
                if (page != 0) f.PreviewPage(page);
                for (int i = 0; i < 6; i++) { Application.DoEvents(); System.Threading.Thread.Sleep(60); }
                Rectangle rc = f.ClientRectangle;
                using (Bitmap bmp = new Bitmap(rc.Width, rc.Height))
                {
                    f.DrawToBitmap(bmp, new Rectangle(0, 0, rc.Width, rc.Height));
                    bmp.Save(outPath, ImageFormat.Png);
                }
                f.Close();
            }
            } catch (Exception ex) {
                System.IO.File.WriteAllText(outPath + ".err.txt", ex.ToString());
            }
        }
    }
}
