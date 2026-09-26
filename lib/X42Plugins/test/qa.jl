using SciMLTesting, X42Plugins

# Docs env does not depend on this sublibrary yet, so rendering is unchecked.
run_qa(X42Plugins; api_docs_kwargs = (; rendered = false))
