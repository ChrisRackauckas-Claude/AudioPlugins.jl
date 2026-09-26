using SciMLTesting, LSPPlugins

# Docs env does not depend on this sublibrary yet, so rendering is unchecked.
run_qa(LSPPlugins; api_docs_kwargs = (; rendered = false))
