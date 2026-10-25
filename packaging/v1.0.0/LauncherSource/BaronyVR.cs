using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

internal static class BaronyVR
{
    [STAThread]
    private static int Main()
    {
        try
        {
            string root = AppDomain.CurrentDomain.BaseDirectory;
            string script = Path.Combine(root, "Launch.ps1");
            if (!File.Exists(script))
                throw new FileNotFoundException(
                    "Barony VR launcher files are missing. Run the Barony VR installer again.");

            string windows = Environment.GetFolderPath(
                Environment.SpecialFolder.Windows);
            string powershell = Path.Combine(windows,
                @"System32\WindowsPowerShell\v1.0\powershell.exe");

            ProcessStartInfo start = new ProcessStartInfo();
            start.FileName = powershell;
            start.Arguments =
                "-NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File \""
                + script + "\"";
            start.WorkingDirectory = root;
            start.UseShellExecute = false;
            start.CreateNoWindow = true;

            using (Process child = Process.Start(start))
            {
                if (child == null)
                    throw new InvalidOperationException("Could not start Barony VR.");
                child.WaitForExit();
                return child.ExitCode;
            }
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "Barony VR",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}