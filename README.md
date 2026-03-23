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

Automatic-Assertion-Generator/
│
├── config.json # Model configuration file
├── layer modules # Multi-layer processing pipeline
├── reports/ # Generated assertion reports
├── scripts/ # Execution scripts
└── README.md
---

## Technologies Used

- Python
- Large Language Models (Groq / Gemini)
- JSON configuration pipeline
- Prompt-based semantic processing

---

## How to Run

1. Clone the repository:


git clone https://github.com/meow1724/Automatic-Assertion-Generator.git

cd Automatic-Assertion-Generator


2. Configure API keys locally in `config.local.json`

3. Run the pipeline script:


python main.py


---

## Learning Outcomes

Through this project:

- Built an LLM-driven verification workflow
- Designed a layered semantic validation pipeline
- Automated assertion generation from specifications
- Improved reliability using cross-model validation

---
