# Zenith

A cross-platform desktop application for orchestrating native LLM harnesses.

Create multiple threads within each project and switch between them while turns are
running. Turns in different threads run concurrently, including when they use the same
provider. Each thread keeps its own draft, attachments, and provider/model selection;
provider and model choices are restored when the application restarts.
