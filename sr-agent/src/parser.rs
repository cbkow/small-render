use regex::Regex;

use crate::messages::{CompletionPatternDef, OutputConfig, ProgressSpec};

enum CompiledPattern {
    Fraction {
        regex: Regex,
        num_group: usize,
        den_group: usize,
    },
    Percentage {
        regex: Regex,
        group: usize,
    },
}

pub struct ProgressParser {
    patterns: Vec<CompiledPattern>,
}

impl ProgressParser {
    pub fn new(spec: &ProgressSpec) -> Self {
        let mut patterns = Vec::new();
        for def in &spec.patterns {
            let regex = match Regex::new(&def.regex) {
                Ok(r) => r,
                Err(e) => {
                    log::warn!("Invalid progress regex '{}': {}", def.regex, e);
                    continue;
                }
            };
            match def.pattern_type.as_str() {
                "fraction" => {
                    patterns.push(CompiledPattern::Fraction {
                        regex,
                        num_group: def.numerator_group as usize,
                        den_group: def.denominator_group as usize,
                    });
                }
                "percentage" => {
                    patterns.push(CompiledPattern::Percentage {
                        regex,
                        group: def.group as usize,
                    });
                }
                other => {
                    log::warn!("Unknown progress pattern type: '{}'", other);
                }
            }
        }
        Self { patterns }
    }

    /// Parse a line of stdout for progress. Returns 0.0-100.0 on match.
    pub fn parse_line(&self, line: &str) -> Option<f32> {
        for pat in &self.patterns {
            match pat {
                CompiledPattern::Fraction {
                    regex,
                    num_group,
                    den_group,
                } => {
                    if let Some(caps) = regex.captures(line) {
                        let num: f32 = caps.get(*num_group)?.as_str().parse().ok()?;
                        let den: f32 = caps.get(*den_group)?.as_str().parse().ok()?;
                        if den > 0.0 {
                            return Some((num / den) * 100.0);
                        }
                    }
                }
                CompiledPattern::Percentage { regex, group } => {
                    if let Some(caps) = regex.captures(line) {
                        let pct: f32 = caps.get(*group)?.as_str().parse().ok()?;
                        return Some(pct);
                    }
                }
            }
        }
        None
    }
}

pub struct OutputParser {
    regex: Regex,
    group: usize,
}

impl OutputParser {
    pub fn new(config: &OutputConfig) -> Option<Self> {
        let pattern = config.regex.as_ref()?;
        if pattern.is_empty() {
            return None;
        }
        let regex = match Regex::new(pattern) {
            Ok(r) => r,
            Err(e) => {
                log::warn!("Invalid output regex '{}': {}", pattern, e);
                return None;
            }
        };
        Some(Self {
            regex,
            group: config.capture_group as usize,
        })
    }

    pub fn parse_line(&self, line: &str) -> Option<String> {
        let caps = self.regex.captures(line)?;
        Some(caps.get(self.group)?.as_str().to_string())
    }
}

pub struct CompletionParser {
    regex: Regex,
}

impl CompletionParser {
    pub fn new(def: &CompletionPatternDef) -> Option<Self> {
        if def.regex.is_empty() {
            return None;
        }
        let regex = match Regex::new(&def.regex) {
            Ok(r) => r,
            Err(e) => {
                log::warn!("Invalid completion regex '{}': {}", def.regex, e);
                return None;
            }
        };
        Some(Self { regex })
    }

    pub fn matches(&self, line: &str) -> bool {
        self.regex.is_match(line)
    }
}
