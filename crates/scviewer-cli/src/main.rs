//! Thin verification CLI for the UI-independent viewer core.
use anyhow::{Result, ensure};
use clap::{Args, Parser, Subcommand, ValueEnum};
use scviewer_core::{DecomposedOptions, GlbOptions, Materials, Session};
use std::{path::PathBuf, time::Instant};

#[derive(Parser)]
#[command(version, about)]
struct Cli {
    #[arg(long, global = true)]
    p4k: Option<PathBuf>,
    #[arg(long, global = true, default_value = "./cache/")]
    cache: PathBuf,
    #[arg(long, global = true)]
    no_cache: bool,
    #[command(subcommand)]
    command: Command,
}
#[derive(Subcommand)]
enum Command {
    Lights {
        entity: String,
    },
    Clips {
        entity: String,
    },
    Search {
        q: String,
        #[arg(long, default_value_t = 20)]
        limit: usize,
    },
    Glb {
        entity: String,
        out: PathBuf,
        #[command(flatten)]
        opts: Options,
    },
    Decomposed {
        entity: String,
        out_root: PathBuf,
        #[command(flatten)]
        opts: Options,
    },
    DecomposedTar {
        entity: String,
        out: PathBuf,
        #[command(flatten)]
        opts: Options,
    },
    Validate {
        dir: PathBuf,
    },
    Ls {
        dir: String,
    },
}
#[derive(Clone, Copy, ValueEnum)]
enum MaterialArg {
    Textures,
    Colors,
    None,
}
#[derive(Args)]
struct Options {
    #[arg(long, default_value_t = 1)]
    lod: u32,
    #[arg(long, default_value_t = 2)]
    mip: u32,
    #[arg(long, value_enum, default_value = "textures")]
    materials: MaterialArg,
    #[arg(long)]
    no_attachments: bool,
    #[arg(long)]
    no_interior: bool,
    #[arg(long)]
    no_lights: bool,
    #[arg(long)]
    no_animations: bool,
    #[arg(long)]
    shields: bool,
    #[arg(long, action = clap::ArgAction::Set)]
    apply_default_pose: Option<bool>,
}
impl Options {
    fn options(&self, decomposed: bool) -> GlbOptions {
        GlbOptions {
            lod: self.lod,
            mip: self.mip,
            materials: match self.materials {
                MaterialArg::Textures => Materials::Textures,
                MaterialArg::Colors => Materials::Colors,
                MaterialArg::None => Materials::None,
            },
            attachments: !self.no_attachments,
            interior: !self.no_interior,
            lights: !self.no_lights,
            shields: self.shields,
            include_animations: !self.no_animations,
            apply_default_pose: self.apply_default_pose.unwrap_or(!decomposed),
        }
    }
}
fn main() -> Result<()> {
    let started = Instant::now();
    let cli = Cli::parse();
    if let Command::Validate { dir } = &cli.command {
        let report = scviewer_core::validate_decomposed_export(dir)?;
        println!("{report:#?}");
        ensure!(report.is_valid, "decomposed validation failed");
        return Ok(());
    }
    let session = Session::open(
        cli.p4k.as_deref(),
        if cli.no_cache { None } else { Some(&cli.cache) },
    )?;
    match cli.command {
        Command::Lights { entity } => {
            let entity = resolve_entity(&session, &entity)?;
            let catalogue = session.lights(&entity)?;
            eprintln!(
                "{}: {} lights, {} MultiLight ports",
                catalogue.entity,
                catalogue.lights.len(),
                catalogue.ports.len()
            );
            println!("{}", serde_json::to_string_pretty(&catalogue)?);
        }
        Command::Clips { entity } => {
            let entity = resolve_entity(&session, &entity)?;
            let call_started = Instant::now();
            let catalogue = session.clip_catalogue(&entity)?;
            eprintln!(
                "{}: {} clips, {} skeletons; catalogue {:.3} ms",
                catalogue.entity,
                catalogue.clips.len(),
                catalogue.skeletons.len(),
                call_started.elapsed().as_secs_f64() * 1000.0
            );
            println!("{}", serde_json::to_string_pretty(&catalogue)?);
        }
        Command::Search { q, limit } => {
            for hit in session.search_entities(&q, limit) {
                println!(
                    "{}\t{}\t{}\t{}",
                    hit.name,
                    hit.guid,
                    hit.category,
                    hit.display_name.as_deref().unwrap_or("")
                );
            }
        }
        Command::Ls { dir } => {
            for entry in session.list_dir(&dir) {
                println!(
                    "{}\t{}\t{}",
                    if entry.is_dir { "dir" } else { "file" },
                    entry.size,
                    entry.path
                );
            }
        }
        Command::Glb { entity, out, opts } => write(
            &out,
            &session.export_entity_glb(
                &resolve_entity(&session, &entity)?,
                &opts.options(false),
                None,
            )?,
        )?,
        Command::Decomposed {
            entity,
            out_root,
            opts,
        } => {
            let result = session.export_entity_decomposed(
                &resolve_entity(&session, &entity)?,
                &out_root,
                &DecomposedOptions {
                    geometry: opts.options(true),
                },
                None,
            )?;
            println!(
                "{} files written to {}",
                result.files.len(),
                out_root.display()
            );
        }
        Command::DecomposedTar { entity, out, opts } => write(
            &out,
            &session.export_entity_decomposed_mem(
                &resolve_entity(&session, &entity)?,
                &DecomposedOptions {
                    geometry: opts.options(true),
                },
                None,
            )?,
        )?,
        Command::Validate { .. } => unreachable!(),
    }
    eprintln!(
        "cache: {} ({:.1} ms)",
        match session.cache_status() {
            scviewer_core::CacheStatus::Hit => "hit",
            scviewer_core::CacheStatus::Miss => "miss",
        },
        started.elapsed().as_secs_f64() * 1000.0
    );
    Ok(())
}
fn write(path: &std::path::Path, bytes: &[u8]) -> Result<()> {
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(path, bytes)?;
    println!("{} bytes written to {}", bytes.len(), path.display());
    Ok(())
}

fn resolve_entity(session: &Session, query: &str) -> Result<String> {
    let hits = session.search_entities(query, usize::MAX);
    hits.iter()
        .find(|hit| hit.name.eq_ignore_ascii_case(query))
        .or_else(|| {
            hits.iter()
                .find(|hit| hit.name.to_lowercase().contains(&query.to_lowercase()))
        })
        .map(|hit| hit.name.clone())
        .ok_or_else(|| anyhow::anyhow!("no entity matching {query:?}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cli_defaults_match_the_core_and_ffi_decomposed_contract() {
        // scbridge-ffi independently checks sc_default_options against this
        // same core value; parsing exercises Clap defaults, not a copied struct.
        for command in ["decomposed", "decomposed-tar"] {
            let cli = Cli::try_parse_from(["scviewer", command, "synthetic", "out"]).unwrap();
            let opts = match cli.command {
                Command::Decomposed { opts, .. } | Command::DecomposedTar { opts, .. } => opts,
                _ => unreachable!(),
            };
            assert_eq!(opts.options(true), DecomposedOptions::default().geometry);
        }
    }
}
