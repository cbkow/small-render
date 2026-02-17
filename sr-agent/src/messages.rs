use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// --- Monitor → Agent ---

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum MonitorToAgent {
    Ping,
    Shutdown,
    Task(TaskMessage),
    Abort(AbortMessage),
}

#[derive(Debug, Deserialize)]
pub struct TaskMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
    pub command: CommandSpec,
    #[serde(default)]
    pub working_dir: Option<String>,
    #[serde(default)]
    pub environment: HashMap<String, String>,
    pub progress: Option<ProgressSpec>,
    pub output_detection: Option<OutputConfig>,
    pub timeout_seconds: Option<u64>,
}

#[derive(Debug, Deserialize)]
pub struct CommandSpec {
    pub executable: String,
    pub args: Vec<String>,
}

#[derive(Debug, Deserialize)]
pub struct ProgressSpec {
    #[serde(default)]
    pub patterns: Vec<ProgressPatternDef>,
    #[serde(default)]
    pub completion_pattern: Option<CompletionPatternDef>,
    #[serde(default)]
    pub error_patterns: Vec<ErrorPatternDef>,
}

fn default_1() -> u32 { 1 }
fn default_2() -> u32 { 2 }

#[derive(Debug, Deserialize)]
pub struct ProgressPatternDef {
    pub regex: String,
    #[serde(rename = "type")]
    pub pattern_type: String,        // "fraction" | "percentage"
    #[serde(default = "default_1")]
    pub numerator_group: u32,
    #[serde(default = "default_2")]
    pub denominator_group: u32,
    #[serde(default = "default_1")]
    pub group: u32,
    #[serde(default)]
    pub info: String,
}

#[derive(Debug, Deserialize)]
pub struct CompletionPatternDef {
    pub regex: String,
    #[serde(default)]
    pub info: String,
}

#[derive(Debug, Deserialize)]
pub struct ErrorPatternDef {
    pub regex: String,
    #[serde(default)]
    pub info: String,
}

#[derive(Debug, Deserialize)]
pub struct OutputConfig {
    pub regex: Option<String>,
    #[serde(default = "default_1")]
    pub capture_group: u32,
}

#[derive(Debug, Deserialize)]
pub struct AbortMessage {
    pub reason: String,
}

// --- Agent → Monitor ---

#[derive(Debug, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum AgentToMonitor {
    Pong,
    Status(StatusMessage),
    Ack(AckMessage),
    Progress(ProgressMessage),
    Stdout(StdoutMessage),
    Completed(CompletedMessage),
    Failed(FailedMessage),
    FrameCompleted(FrameCompletedMessage),
}

#[derive(Debug, Serialize)]
pub struct StatusMessage {
    pub state: String,
    pub pid: u32,
}

#[derive(Debug, Serialize)]
pub struct AckMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
}

#[derive(Debug, Serialize)]
pub struct ProgressMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
    pub progress_pct: f32,
    pub elapsed_ms: u64,
}

#[derive(Debug, Serialize)]
pub struct StdoutMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
    pub lines: Vec<String>,
}

#[derive(Debug, Serialize)]
pub struct CompletedMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
    pub elapsed_ms: u64,
    pub exit_code: i32,
    pub output_file: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct FailedMessage {
    pub job_id: String,
    pub frame_start: u32,
    pub frame_end: u32,
    pub exit_code: i32,
    pub error: String,
}

#[derive(Debug, Serialize)]
pub struct FrameCompletedMessage {
    pub job_id: String,
    pub frame: u32,
}
