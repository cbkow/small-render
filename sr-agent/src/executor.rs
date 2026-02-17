use std::io::BufRead;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::messages::TaskMessage;
use crate::parser::{CompletionParser, OutputParser, ProgressParser};

pub enum RenderEvent {
    Started,
    Stdout(Vec<String>),
    Progress { pct: f32, elapsed_ms: u64 },
    FrameCompleted { frame: u32 },
    Completed {
        elapsed_ms: u64,
        exit_code: i32,
        output_file: Option<String>,
    },
    Failed {
        exit_code: i32,
        error: String,
    },
}

pub struct RenderExecutor {
    event_rx: mpsc::Receiver<RenderEvent>,
    abort_flag: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
}

const STDOUT_FLUSH_LINES: usize = 50;
const STDOUT_FLUSH_INTERVAL: Duration = Duration::from_secs(5);

impl RenderExecutor {
    pub fn start(task: TaskMessage) -> Result<Self, String> {
        let job_id = task.job_id.clone();
        let frame_start = task.frame_start;
        let frame_end = task.frame_end;

        let mut cmd = Command::new(&task.command.executable);
        cmd.args(&task.command.args);
        cmd.stdout(Stdio::piped());
        cmd.stderr(Stdio::piped());

        if let Some(ref wd) = task.working_dir {
            if !wd.is_empty() {
                cmd.current_dir(wd);
            }
        }

        for (k, v) in &task.environment {
            cmd.env(k, v);
        }

        let mut child = cmd
            .spawn()
            .map_err(|e| format!("Failed to spawn process: {}", e))?;

        let (event_tx, event_rx) = mpsc::channel::<RenderEvent>();
        let abort_flag = Arc::new(AtomicBool::new(false));
        let abort_clone = abort_flag.clone();

        // Build completion parser early — shared between stdout loop and stderr thread
        let completion_parser = Arc::new(
            task.progress
                .as_ref()
                .and_then(|spec| spec.completion_pattern.as_ref())
                .and_then(|def| CompletionParser::new(def)),
        );
        let completion_counter = Arc::new(AtomicU32::new(0));

        // Take stderr handle and read on a mini-thread
        let stderr_lines: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let stderr_clone = stderr_lines.clone();
        if let Some(stderr) = child.stderr.take() {
            let cp = completion_parser.clone();
            let cc = completion_counter.clone();
            let tx = event_tx.clone();
            let fs = frame_start;
            let fe = frame_end;
            thread::spawn(move || {
                let reader = std::io::BufReader::new(stderr);
                for line in reader.lines() {
                    match line {
                        Ok(l) => {
                            // Check completion pattern on stderr lines
                            if let Some(ref parser) = *cp {
                                if parser.matches(&l) {
                                    let count = cc.fetch_add(1, Ordering::SeqCst);
                                    let frame = fs + count;
                                    if frame <= fe {
                                        let _ = tx.send(RenderEvent::FrameCompleted { frame });
                                    }
                                }
                            }
                            if let Ok(mut buf) = stderr_clone.lock() {
                                buf.push(l);
                            }
                        }
                        Err(_) => break,
                    }
                }
            });
        }

        // Build other parsers
        let progress_parser = task
            .progress
            .as_ref()
            .map(|spec| ProgressParser::new(spec));

        let output_parser = task
            .output_detection
            .as_ref()
            .and_then(|cfg| OutputParser::new(cfg));

        let timeout = task.timeout_seconds.map(Duration::from_secs);

        let worker = thread::spawn(move || {
            worker_func(
                child,
                event_tx,
                abort_clone,
                progress_parser,
                output_parser,
                completion_parser,
                completion_counter,
                stderr_lines,
                timeout,
                job_id.clone(),
                frame_start,
                frame_end,
            );
        });

        Ok(Self {
            event_rx,
            abort_flag,
            worker: Some(worker),
            job_id: task.job_id,
            frame_start: task.frame_start,
            frame_end: task.frame_end,
        })
    }

    /// Non-blocking poll for render events.
    pub fn poll_events(&self) -> Vec<RenderEvent> {
        let mut events = Vec::new();
        while let Ok(ev) = self.event_rx.try_recv() {
            events.push(ev);
        }
        events
    }

    /// Signal the render process to abort.
    pub fn abort(&self) {
        self.abort_flag.store(true, Ordering::SeqCst);
    }

    /// Check if the worker thread has finished.
    pub fn is_done(&self) -> bool {
        match &self.worker {
            Some(w) => w.is_finished(),
            None => true,
        }
    }
}

fn worker_func(
    mut child: Child,
    tx: mpsc::Sender<RenderEvent>,
    abort_flag: Arc<AtomicBool>,
    progress_parser: Option<ProgressParser>,
    output_parser: Option<OutputParser>,
    completion_parser: Arc<Option<CompletionParser>>,
    completion_counter: Arc<AtomicU32>,
    stderr_lines: Arc<Mutex<Vec<String>>>,
    timeout: Option<Duration>,
    _job_id: String,
    frame_start: u32,
    frame_end: u32,
) {
    let start_time = Instant::now();
    let _ = tx.send(RenderEvent::Started);

    let stdout = match child.stdout.take() {
        Some(s) => s,
        None => {
            let _ = tx.send(RenderEvent::Failed {
                exit_code: -1,
                error: "Failed to capture stdout".into(),
            });
            return;
        }
    };

    let reader = std::io::BufReader::new(stdout);
    let mut stdout_buf: Vec<String> = Vec::new();
    let mut last_flush = Instant::now();
    let mut last_output_file: Option<String> = None;

    for line in reader.lines() {
        // Check abort
        if abort_flag.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            flush_stdout(&tx, &mut stdout_buf, &stderr_lines);
            let _ = tx.send(RenderEvent::Failed {
                exit_code: -1,
                error: "Aborted by monitor".into(),
            });
            return;
        }

        // Check timeout
        if let Some(t) = timeout {
            if start_time.elapsed() > t {
                let _ = child.kill();
                let _ = child.wait();
                flush_stdout(&tx, &mut stdout_buf, &stderr_lines);
                let _ = tx.send(RenderEvent::Failed {
                    exit_code: -1,
                    error: format!("Timeout after {}s", t.as_secs()),
                });
                return;
            }
        }

        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };

        // Parse progress
        if let Some(ref parser) = progress_parser {
            if let Some(pct) = parser.parse_line(&line) {
                let _ = tx.send(RenderEvent::Progress {
                    pct,
                    elapsed_ms: start_time.elapsed().as_millis() as u64,
                });
            }
        }

        // Parse output file
        if let Some(ref parser) = output_parser {
            if let Some(path) = parser.parse_line(&line) {
                last_output_file = Some(path);
            }
        }

        // Parse per-frame completion (stdout — stderr is checked by its own thread)
        if let Some(ref parser) = *completion_parser {
            if parser.matches(&line) {
                let count = completion_counter.fetch_add(1, Ordering::SeqCst);
                let frame = frame_start + count;
                if frame <= frame_end {
                    let _ = tx.send(RenderEvent::FrameCompleted { frame });
                }
            }
        }

        stdout_buf.push(line);

        // Flush on threshold
        if stdout_buf.len() >= STDOUT_FLUSH_LINES
            || last_flush.elapsed() >= STDOUT_FLUSH_INTERVAL
        {
            flush_stdout(&tx, &mut stdout_buf, &stderr_lines);
            last_flush = Instant::now();
        }
    }

    // Flush remaining stdout + stderr
    flush_stdout(&tx, &mut stdout_buf, &stderr_lines);

    // Wait for process to exit
    let status = match child.wait() {
        Ok(s) => s,
        Err(e) => {
            let _ = tx.send(RenderEvent::Failed {
                exit_code: -1,
                error: format!("Failed to wait for process: {}", e),
            });
            return;
        }
    };

    let elapsed_ms = start_time.elapsed().as_millis() as u64;
    let exit_code = status.code().unwrap_or(-1);

    if status.success() {
        let _ = tx.send(RenderEvent::Completed {
            elapsed_ms,
            exit_code,
            output_file: last_output_file,
        });
    } else {
        let _ = tx.send(RenderEvent::Failed {
            exit_code,
            error: format!("Process exited with code {}", exit_code),
        });
    }
}

fn flush_stdout(
    tx: &mpsc::Sender<RenderEvent>,
    stdout_buf: &mut Vec<String>,
    stderr_lines: &Arc<Mutex<Vec<String>>>,
) {
    // Drain stderr into stdout buffer
    if let Ok(mut errs) = stderr_lines.lock() {
        if !errs.is_empty() {
            for line in errs.drain(..) {
                stdout_buf.push(format!("[stderr] {}", line));
            }
        }
    }

    if stdout_buf.is_empty() {
        return;
    }

    let lines: Vec<String> = stdout_buf.drain(..).collect();
    let _ = tx.send(RenderEvent::Stdout(lines));
}
