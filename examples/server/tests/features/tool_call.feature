@llama.cpp
@server
Feature: llama.cpp server

  Background: Server startup
    Given a server listening on localhost:8080
    And   a model file tinyllamas/stories260K.gguf from HF repo ggml-org/models
    And   a model file test-model.gguf
    And   a model alias tinyllama-2
    And   BOS token is 1
    And   42 as server seed
    And   8192 KV cache size
    And   32 as batch size
    And   2 slots
    And   64 server max tokens to predict
    And   prometheus compatible metrics exposed
    And   jinja templates are enabled

  @wip
  Scenario Outline: OAI Compatibility w/ required tool
    Given a chat template file ../../../tests/chat/templates/<template_name>.jinja
    And   the server is starting
    And   the server is healthy
    And   a model test
    And   <n> max tokens to predict
    And   a user prompt write a hello world in python
    And   a tool choice <tool_choice>
    And   tools <tools>
    And   an OAI compatible chat completions request with no api error
    Then  tool <tool_name> is called with arguments <tool_arguments>

    Examples: Prompts
      | template_name                         | n   | tool_name | tool_arguments       | tool_choice | tools |
      | meta-llama-Meta-Llama-3.1-8B-Instruct | 64  | test      | {}                   | required    | [{"type":"function", "function": {"name": "test", "description": "", "parameters": {"type": "object", "properties": {}}}}] |
      | meta-llama-Meta-Llama-3.1-8B-Instruct | 16  | ipython   | {"code": "it and "}  | required    | [{"type":"function", "function": {"name": "ipython", "description": "", "parameters": {"type": "object", "properties": {"code": {"type": "string", "description": ""}}, "required": ["code"]}}}] |
      | meetkai-functionary-medium-v3.2       | 64  | test      | {}                   | required    | [{"type":"function", "function": {"name": "test", "description": "", "parameters": {"type": "object", "properties": {}}}}] |
      | meetkai-functionary-medium-v3.2       | 64  | ipython   | {"code": "Yes,"}     | required    | [{"type":"function", "function": {"name": "ipython", "description": "", "parameters": {"type": "object", "properties": {"code": {"type": "string", "description": ""}}, "required": ["code"]}}}] |

  Scenario: OAI Compatibility w/ no tool
    Given a chat template file ../../../tests/chat/templates/meta-llama-Meta-Llama-3.1-8B-Instruct.jinja
    And   the server is starting
    And   the server is healthy
    And   a model test
    And   16 max tokens to predict
    And   a user prompt write a hello world in python
    And   a tool choice <tool_choice>
    And   tools []
    And   an OAI compatible chat completions request with no api error
    Then  no tool is called
