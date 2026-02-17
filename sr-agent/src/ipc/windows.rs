use std::io::{self, Read, Write};
use std::thread;
use std::time::Duration;

use windows::core::HSTRING;
use windows::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_SHARE_NONE, OPEN_EXISTING,
    ReadFile, WriteFile,
};
use windows::Win32::System::Pipes::{PeekNamedPipe, WaitNamedPipeW};

/// Named pipe client for communicating with the monitor.
pub struct WindowsPipeClient {
    handle: HANDLE,
}

impl WindowsPipeClient {
    /// Connect to the monitor's named pipe for the given node_id.
    /// Retries up to 3 times with 3-second intervals.
    pub fn connect(node_id: &str) -> io::Result<Self> {
        let pipe_name = format!(r"\\.\pipe\SmallRenderAgent_{}", node_id);
        let pipe_name_h = HSTRING::from(&pipe_name);

        let max_attempts = 3;
        let retry_delay = Duration::from_secs(3);

        for attempt in 1..=max_attempts {
            log::info!("Connecting to pipe: {} (attempt {}/{})", pipe_name, attempt, max_attempts);

            // Wait for pipe to become available (up to 5 seconds)
            let wait_ok = unsafe { WaitNamedPipeW(&pipe_name_h, 5000) }.as_bool();
            if !wait_ok && attempt < max_attempts {
                log::warn!("Pipe not available yet, retrying in {}s...", retry_delay.as_secs());
                thread::sleep(retry_delay);
                continue;
            }

            let handle = unsafe {
                CreateFileW(
                    &pipe_name_h,
                    (FILE_GENERIC_READ | FILE_GENERIC_WRITE).0,
                    FILE_SHARE_NONE,
                    None,
                    OPEN_EXISTING,
                    Default::default(),
                    None,
                )
            };

            match handle {
                Ok(h) if h != INVALID_HANDLE_VALUE => {
                    log::info!("Connected to monitor pipe");
                    return Ok(Self { handle: h });
                }
                Ok(_) => {
                    let err = io::Error::last_os_error();
                    if attempt < max_attempts {
                        log::warn!("CreateFileW returned INVALID_HANDLE_VALUE: {}, retrying...", err);
                        thread::sleep(retry_delay);
                    } else {
                        return Err(err);
                    }
                }
                Err(e) => {
                    if attempt < max_attempts {
                        log::warn!("CreateFileW failed: {}, retrying...", e);
                        thread::sleep(retry_delay);
                    } else {
                        return Err(io::Error::new(io::ErrorKind::ConnectionRefused, e.to_string()));
                    }
                }
            }
        }

        Err(io::Error::new(
            io::ErrorKind::ConnectionRefused,
            format!("Failed to connect to pipe {} after {} attempts", pipe_name, max_attempts),
        ))
    }
}

impl WindowsPipeClient {
    /// Check how many bytes are available to read without blocking.
    pub fn peek_available(&self) -> io::Result<usize> {
        let mut available = 0u32;
        unsafe {
            PeekNamedPipe(self.handle, None, 0, None, Some(&mut available), None)
                .map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))?;
        }
        Ok(available as usize)
    }
}

impl Read for WindowsPipeClient {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut bytes_read = 0u32;
        unsafe {
            ReadFile(self.handle, Some(buf), Some(&mut bytes_read), None)
                .map_err(|e| io::Error::new(io::ErrorKind::BrokenPipe, e.to_string()))?;
        }
        Ok(bytes_read as usize)
    }
}

impl Write for WindowsPipeClient {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut bytes_written = 0u32;
        unsafe {
            WriteFile(self.handle, Some(buf), Some(&mut bytes_written), None)
                .map_err(|e| io::Error::new(io::ErrorKind::BrokenPipe, e.to_string()))?;
        }
        Ok(bytes_written as usize)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(()) // Named pipes don't need explicit flushing
    }
}

impl Drop for WindowsPipeClient {
    fn drop(&mut self) {
        if !self.handle.is_invalid() {
            unsafe { let _ = CloseHandle(self.handle); }
        }
    }
}
