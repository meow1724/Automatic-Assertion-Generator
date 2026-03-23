# Automatic Assertion Generator

This project implements an **Automatic Assertion Generator** that generates formal verification assertions from natural-language specifications using Large Language Models (LLMs). It applies a multi-layer validation pipeline to improve correctness, consistency, and reliability of generated assertions.

The system is designed to assist hardware designers and verification engineers by reducing manual effort in writing assertions for formal verification workflows.

---

## Features

- Converts natural-language specifications into formal assertions
- Multi-layer LLM-based validation pipeline
- Uses multiple models for generation and verification
- Configurable architecture via JSON configuration
- Improves assertion accuracy through iterative refinement
- Modular and extensible design

---

## Architecture Overview

The system follows a layered processing pipeline:

1. **Specification Processing Layer**
   - Extracts semantic meaning from input text

2. **Assertion Generation Layer**
   - Generates candidate assertions using LLMs

3. **Validation Layer**
   - Cross-checks assertions using secondary models

4. **Subsumption Analysis Layer**
   - Removes redundant or conflicting assertions

---

## Project Structure
