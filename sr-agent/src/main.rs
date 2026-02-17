mod executor;
mod ipc;
mod messages;
mod parser;
mod watchdog;

use clap::Parser;
use executor::{RenderEvent, RenderExecutor};
use messages::{
    AckMessage, AgentToMonitor, CompletedMessage, FailedMessage, FrameCompletedMessage,
    MonitorToAgent, ProgressMessage, StatusMessage, StdoutMessage,
};
use std::process;
use std::thread;
use std::time::Duration;

#[derive(Parser)]
#[command(name = "sr-agent", about = "SmallRender headless render agent", version)]
struct Args {
    /// Node ID to connect to (must match the monitor's node ID)
    #[arg(long)]
    node_id: String,
}

/// Holds the named mutex handle to keep it alive for the process lifetime.
#[cfg(windows)]
struct MutexGuard {
    _handle: windows::Win32::Foundation::HANDLE,
}

#[cfg(not(windows))]
struct MutexGuard;

/// Creates a named mutex to prevent duplicate agents for the same node.
/// Exits with error if another agent is already running.
#[cfg(windows)]
fn ensure_single_instance(node_id: &str) -> MutexGuard {
    use windows::core::PCWSTR;
    use windows::Win32::Foundation::GetLastError;
    use windows::Win32::Foundation::ERROR_ALREADY_EXISTS;
    use windows::Win32::System::Threading::CreateMutexW;

    let mutex_name: Vec<u16> = format!("SmallRenderAgent_{}\0", node_id)
        .encode_utf16()
        .collect();

    let handle = unsafe { CreateMutexW(None, false, PCWSTR(mutex_name.as_ptr())) };
    match handle {
        Ok(h) => {
            if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
                log::error!("Another sr-agent is already running for node_id={}", node_id);
                process::exit(1);
            }
            MutexGuard { _handle: h }
        }
        Err(e) => {
            log::error!("Failed to create instance mutex: {}", e);
            process::exit(1);
        }
    }
}

#[cfg(not(windows))]
fn ensure_single_instance(_node_id: &str) -> MutexGuard {
    MutexGuard
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let args = Args::parse();
    log::info!("sr-agent starting for node_id={}", args.node_id);

    // Single instance check — prevent duplicate agents for the same node
    let _mutex_guard = ensure_single_instance(&args.node_id);

    let mut pipe = match ipc::PipeClient::connect(&args.node_id) {
        Ok(p) => p,
        Err(e) => {
            log::error!("Failed to connect to monitor: {}", e);
            process::exit(1);
        }
    };

    // Send initial status: idle + our PID
    let status = AgentToMonitor::Status(StatusMessage {
        state: "idle".into(),
        pid: process::id(),
    });
    if let Err(e) = send_message(&mut pipe, &status) {
        log::error!("Failed to send initial status: {}", e);
        process::exit(1);
    }
    log::info!("Sent initial status (pid={})", process::id());

    let mut active_render: Option<RenderExecutor> = None;

    loop {
        if let Some(ref executor) = active_render {
            // === RENDERING MODE ===
            let mut done = false;

            // 1. Process render events (non-blocking)
            for event in executor.poll_events() {
                match event {
                    RenderEvent::Started => {
                        log::info!(
                            "Render started: job={} chunk={}-{}",
                            executor.job_id,
                            executor.frame_start,
                            executor.frame_end,
                        );
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::Ack(AckMessage {
                                job_id: executor.job_id.clone(),
                                frame_start: executor.frame_start,
                                frame_end: executor.frame_end,
                            }),
                        );
                    }
                    RenderEvent::Stdout(lines) => {
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::Stdout(StdoutMessage {
                                job_id: executor.job_id.clone(),
                                frame_start: executor.frame_start,
                                frame_end: executor.frame_end,
                                lines,
                            }),
                        );
                    }
                    RenderEvent::Progress { pct, elapsed_ms } => {
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::Progress(ProgressMessage {
                                job_id: executor.job_id.clone(),
                                frame_start: executor.frame_start,
                                frame_end: executor.frame_end,
                                progress_pct: pct,
                                elapsed_ms,
                            }),
                        );
                    }
                    RenderEvent::FrameCompleted { frame } => {
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::FrameCompleted(FrameCompletedMessage {
                                job_id: executor.job_id.clone(),
                                frame,
                            }),
                        );
                    }
                    RenderEvent::Completed {
                        elapsed_ms,
                        exit_code,
                        output_file,
                    } => {
                        log::info!(
                            "Render completed: job={} chunk={}-{} exit_code={} elapsed={}ms",
                            executor.job_id,
                            executor.frame_start,
                            executor.frame_end,
                            exit_code,
                            elapsed_ms,
                        );
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::Completed(CompletedMessage {
                                job_id: executor.job_id.clone(),
                                frame_start: executor.frame_start,
                                frame_end: executor.frame_end,
                                elapsed_ms,
                                exit_code,
                                output_file,
                            }),
                        );
                        done = true;
                    }
                    RenderEvent::Failed { exit_code, error } => {
                        log::warn!(
                            "Render failed: job={} chunk={}-{} exit_code={} error={}",
                            executor.job_id,
                            executor.frame_start,
                            executor.frame_end,
                            exit_code,
                            error,
                        );
                        let _ = send_message(
                            &mut pipe,
                            &AgentToMonitor::Failed(FailedMessage {
                                job_id: executor.job_id.clone(),
                                frame_start: executor.frame_start,
                                frame_end: executor.frame_end,
                                exit_code,
                                error,
                            }),
                        );
                        done = true;
                    }
                }
            }

            if done {
                active_render = None;
                let _ = send_message(
                    &mut pipe,
                    &AgentToMonitor::Status(StatusMessage {
                        state: "idle".into(),
                        pid: process::id(),
                    }),
                );
                continue;
            }

            // 2. Check IPC messages (non-blocking via peek)
            let has_data = pipe.peek_available().unwrap_or(0) > 0;
            if has_data {
                match ipc::read_message(&mut pipe) {
                    Ok(payload) => {
                        if let Ok(msg) = serde_json::from_slice::<MonitorToAgent>(&payload) {
                            match msg {
                                MonitorToAgent::Ping => {
                                    let _ = send_message(&mut pipe, &AgentToMonitor::Pong);
                                }
                                MonitorToAgent::Shutdown => {
                                    log::info!("Received shutdown during render, aborting");
                                    if let Some(ref exec) = active_render {
                                        exec.abort();
                                    }
                                    // Wait briefly for worker to finish
                                    thread::sleep(Duration::from_millis(500));
                                    break;
                                }
                                MonitorToAgent::Abort(abort) => {
                                    log::info!("Received abort: {}", abort.reason);
                                    if let Some(ref exec) = active_render {
                                        exec.abort();
                                    }
                                }
                                MonitorToAgent::Task(_) => {
                                    log::warn!("Received task while already rendering, ignoring");
                                }
                            }
                        }
                    }
                    Err(e) => {
                        log::error!("Pipe read error during render: {}", e);
                        if let Some(ref exec) = active_render {
                            exec.abort();
                        }
                        break;
                    }
                }
            }

            thread::sleep(Duration::from_millis(100));
        } else {
            // === IDLE MODE === (blocking read)
            let payload = match ipc::read_message(&mut pipe) {
                Ok(data) => data,
                Err(e) => {
                    log::error!("Pipe read error (monitor disconnected?): {}", e);
                    break;
                }
            };

            let msg: MonitorToAgent = match serde_json::from_slice(&payload) {
                Ok(m) => m,
                Err(e) => {
                    log::warn!("Failed to parse message: {}", e);
                    continue;
                }
            };

            match msg {
                MonitorToAgent::Ping => {
                    log::debug!("Received ping, sending pong");
                    if let Err(e) = send_message(&mut pipe, &AgentToMonitor::Pong) {
                        log::error!("Failed to send pong: {}", e);
                        break;
                    }
                }
                MonitorToAgent::Shutdown => {
                    log::info!("Received shutdown command, exiting");
                    break;
                }
                MonitorToAgent::Task(task) => {
                    log::info!(
                        "Received task: job={} chunk={}-{} cmd={}",
                        task.job_id,
                        task.frame_start,
                        task.frame_end,
                        task.command.executable,
                    );
                    match RenderExecutor::start(task) {
                        Ok(executor) => {
                            let _ = send_message(
                                &mut pipe,
                                &AgentToMonitor::Status(StatusMessage {
                                    state: "rendering".into(),
                                    pid: process::id(),
                                }),
                            );
                            active_render = Some(executor);
                        }
                        Err(e) => {
                            log::error!("Failed to start render: {}", e);
                            // Send failed message — use the info from the task
                            // We can't access task here since it was moved, but the error
                            // message will be logged. The monitor will detect no ack.
                        }
                    }
                }
                MonitorToAgent::Abort(_) => {
                    // Nothing to abort
                }
            }
        }
    }

    log::info!("sr-agent exiting");
}

fn send_message(
    pipe: &mut ipc::PipeClient,
    msg: &AgentToMonitor,
) -> Result<(), Box<dyn std::error::Error>> {
    let payload = serde_json::to_vec(msg)?;
    ipc::write_message(pipe, &payload)?;
    Ok(())
}
