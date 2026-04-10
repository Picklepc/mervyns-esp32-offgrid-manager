# Image Generation Narrative

This project uses AI image generation in a very constrained way: not as an always-on app dependency, but as an optional way to turn a practical storage label into something more playful.

## What This Work Was Trying To Solve

The label maker already needed to do useful local work:

- normalize text
- fit labels to stock size
- preview quickly
- print reliably

The AI path had to fit around that instead of taking control of the product.

So the design constraint was:

- keep the non-AI path fast and local
- treat image generation as an optional enhancement
- keep provider integration thin enough that it can be swapped or removed

## Core Findings

### 1. The label renderer must stand on its own

The standard label path is not a fallback in the sad sense. It is the baseline product behavior.

That matters because it gives the project:

- a useful non-AI mode
- deterministic previews
- no cloud dependency for everyday use
- a stable print path even if upstream image generation fails

### 2. Provider integration should be adapter-shaped, not product-shaped

This repo talks to image generation providers in a small, explicit way:

- OpenAI direct
- Hugging Face direct
- custom endpoint

The provider-specific logic belongs near the HTTP request/response boundary, not smeared across the UI or label engine.

That keeps the system adaptable when:

- a model changes
- a token is missing
- a service fails
- a project wants to swap providers later

### 3. The prompt is part of the product contract

The "Go Crazy" path is not random decoration. It is a structured prompt path that still starts from:

- normalized label text
- shelf hint / placement note
- intended stock size and orientation
- monochrome label reality

That means the prompt builder is not just prompt fluff. It is the translation layer between practical labeling and image-generation behavior.

### 4. It is fine to stop short of a universal abstraction

This repo is not trying to be a generic multimodal framework. It is trying to make a narrow workflow usable.

That is why the integration is valuable to other vibe-coded projects:

- it is small
- it is concrete
- it is close to hardware output

## How To Reuse This In Another Project

If you want optional AI image generation without building your whole app around it:

1. Keep your non-AI renderer working first.
2. Normalize your content before prompt building.
3. Keep provider adapters thin.
4. Return a payload your local preview and print path can already consume.

That last point is the important one. The best AI integration is one that drops into an already-working local render/print workflow.

## Relevant Files

- [src/main.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\main.cpp)
- [src/label_engine.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\label_engine.cpp)
- [src/web_pages.cpp](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\src\web_pages.cpp)
- [include/app_defaults.h](C:\Users\patri\OneDrive\Documents\ESP32\Pack%20Rat\include\app_defaults.h)

## Things Worth Carrying Forward

- provider-agnostic request routing with explicit provider branches
- local preview path that does not care where the image came from
- graceful failure messages when no token or endpoint is configured
- prompt inputs that still reflect real hardware constraints
