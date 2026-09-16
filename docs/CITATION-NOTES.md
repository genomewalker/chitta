# Citation check — 2026-09-16

The [index](CITATIONS.md) supplies the canonical entries and verification status.
Sources support their own methods and experiments; they do not establish that
chitta implements every detail or inherits the reported gains.

## Numbers and attribution

SwarmWorld's HTML §2.6/Figure 20 says: “Approximately 95% of first reuse occurred
through direct physical observation in both conditions.” [14](#ref-14) The
~95% wording in CHANGELOG.md, docs/changelog.html and the learning decision memo
matches. There is no SwarmWorld number in this checkout's README.md.

Section 2.5/Figure 14 also matches the proposal: at tick 3,200, four matched
100-agent seeds give best final artifact means 0.3488 (isolated envelope) and
0.2380 (full culture). Validated-invention means are 5.75 (full culture), 7.00
(no explicit culture), and 2.75 (isolated envelope). These describe that
experiment, not a universal advantage for isolated coding agents. [14](#ref-14)
No numeric replacement or differing-source quotation was necessary.

MetaMaterialsDiscovery confirms three autonomous Fable 5.1 runs from the same
prompt and five reference images. Their generated mechanics models differ;
“model-dependent” here concerns those mechanics models, not a comparison of
three language models. The archive reports different weakened-interface
responses. [15](#ref-15)

The bridge table's +3 percentage-point LongMemEval gain is labelled a hypothesis
for a proposed adaptation of AgenticRag-R1, not a result quoted from that paper.
[53](#ref-53) The local benchmark scores remain local measurements, with the
LongMemEval and LoCoMo papers cited for benchmark provenance. [16](#ref-16)
[17](#ref-17) [18](#ref-18)

## Corrections

- Spisak/Friston: replaced the invented paper title and DOI ending `08696`
  (HTTP 404) with the published title and DOI ending `133472`. The journal year
  is 2026; the arXiv preprint dates to 2025. [19](#ref-19)
- RRF, HNSW and MemGPT: expanded shorthand titles to the source titles; HNSW's
  2020 journal date is retained and distinguished from its 2016 preprint.
  [3](#ref-3) [4](#ref-4) [22](#ref-22)
- Latent Briefing: corrected Ramp Labs (2025) to Ben Geist, April 10, 2026, and
  the full title. It describes KV-cache compaction; it is background for the
  text-compaction discussion, not an implementation-equivalence claim.
  [23](#ref-23)
- Anthropic prompting and Skills pages: replaced two HTTP-404 URLs with the
  current documented routes; corrected the Skills page title. [35](#ref-35)
  [36](#ref-36)
- MARCO evidence: replaced claims that only metadata could be read with the
  fetched description of Retriever, Reasoner, Validator and Synthesizer agents
  and shared memory. The proposal's expected gain remains a hypothesis.
  [58](#ref-58)
- Consolidation Without Weights: the Zenodo description is available and
  distinguishes textual rewriting from parametric isolation; it does not
  license a blanket claim that replay/reindexing inherits CLS guarantees.
  [60](#ref-60)
- Both fidelis records are versioned software archives with descriptions,
  v0.0.94 and v0.0.97. Replaced “title only” claims and claims about chitta's
  live configuration incorrectly attributed to these external DOIs with facts
  about fidelis actually supported by the release descriptions. [61](#ref-61)
  [62](#ref-62)
- Slot-memory poster: title and authors confirmed by the conference and DOI
  registry; publisher body remained inaccessible. Evidence is now explicitly
  title-level rather than asserting an unavailable abstract. [63](#ref-63)
- Astra: retained the matching source but qualified the exact publication day,
  which the fetched page did not expose. [41](#ref-41)

## Method references and limits

Added sources for Wilson intervals, TurboQuant, compact DAWG/DAWG, modern
Hopfield/dense associative memory, SDRs, HDC/VSA binding, MDL, BM25, Sequitur,
Hebbian learning, PPM, temporal differences, Q-learning, classic Hopfield,
Landlock, bubblewrap and the two memory benchmarks. [2](#ref-2) [5](#ref-5)
[6](#ref-6) [67](#ref-67) [7](#ref-7) [8](#ref-8) [9](#ref-9) [10](#ref-10)
[11](#ref-11) [24](#ref-24) [25](#ref-25) [26](#ref-26) [30](#ref-30)
[31](#ref-31) [32](#ref-32) [33](#ref-33) [12](#ref-12) [13](#ref-13)

The DAWG and compact-DAWG papers are distinct; neither citation alone verifies
this implementation's per-symbol amortized complexity. SDR literature supplies
background for the sparse representation, not authorship of chitta's posting
index. Psychological and neural-network inspirations do not validate the
project's chosen scoring multipliers. [1](#ref-1) [20](#ref-20) [21](#ref-21)
[27](#ref-27) [28](#ref-28) [29](#ref-29)

The final named-method pass also added Thompson sampling [69](#ref-69),
PageRank [70](#ref-70), RLM-style exploration [71](#ref-71), CALFW dataset
provenance [72](#ref-72), and LSH [73](#ref-73). These citations describe the
methods, not measured gains from chitta's implementations.

## Protected decisions and unresolved sources

Both decision memo bodies were preserved. Their editor-style code citations
point to another worktree and, in one case, private historical state. Those
historical line targets are not reproducible from this checkout and were not
silently rewritten to current code. No private state or daemon was accessed.
Dated appendices record this limitation and the learning memo's matching
SwarmWorld numbers. The MDL/analogy memo makes no external numerical claim.

The requested Hermes source is Nous Research’s “Refactoring Hermes with 1,393
agents”, published 2026-09. One direct fetch returned HTTP 200 and confirmed
the title and September 2–4, 2026 run dates. The article text does not state
an exact publication day; publication is recorded at month precision as
requested. Structured metadata contains a September 15 timestamp. The browser
open failed before the direct fetch. [65](#ref-65)

Astra's title/content could be fetched with the browser although curl returned
403; the claimed publication day remains unverified. [41](#ref-41) Other failed
publisher requests were resolved using arXiv, the source's publisher metadata
in the browser, or Crossref as stated per row. An HTTP 200 redirect interstitial
was not treated as paper content.

OpenReview forum fetches returned HTTP 200 browser-verification pages; those
were not treated as publication metadata. The modern Hopfield and sparse MoE
entries cite the verified arXiv versions. The local PDF text extractor was
unavailable; browser PDF extraction confirmed the sparse MoE paper's title and
authors, while modern Hopfield's arXiv abstract page supplied its metadata.

## Validation

Final inventory: 73 entries — 60 resolved, 12 metadata/evidence corrections,
1 unresolvable; 26 newly introduced references (the added count overlaps the
verification statuses). The unresolved entry is Astra's exact publication day.
The status log covers 97 URLs, including
replacement targets and failed metadata routes; no response bodies are committed.

Both documentation gates passed with zero errors. The local-link check covered
39 pages and 849 links, preserving 35 historical editor-style citations. The
citation gate checked 240 page reference entries. Shell syntax and whitespace
checks passed. A temporary fixture confirmed that the citation checker rejects
canonical-text drift, unknown claim IDs, missing index rows, missing fetch
records and unmanaged References sections. Both decision bodies remain exact
byte prefixes of the updated files. JSON comparison confirmed that proposal
changes are restricted to evidence arrays. No application code or test fixtures
were changed; no live daemon or private configuration was accessed.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-1"></a>**[1]** John R. Anderson and Lael J. Schooler. Reflections of the Environment in Memory. Psychological Science 2(6), 396–408 (1991). [source](<https://doi.org/10.1111/j.1467-9280.1991.tb00174.x>)
- <a id="ref-2"></a>**[2]** Edwin B. Wilson. Probable Inference, the Law of Succession, and Statistical Inference. Journal of the American Statistical Association 22(158), 209–212 (1927). [source](<https://doi.org/10.1080/01621459.1927.10502953>)
- <a id="ref-3"></a>**[3]** Gordon V. Cormack, Charles L. A. Clarke, and Stefan Buettcher. Reciprocal rank fusion outperforms condorcet and individual rank learning methods. SIGIR, 758–759 (2009). [source](<https://doi.org/10.1145/1571941.1572114>)
- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-5"></a>**[5]** Amir Zandieh, Majid Daliri, Majid Hadian, and Vahab Mirrokni. TurboQuant: Online Vector Quantization with Near-optimal Distortion Rate. arXiv:2504.19874 (2025). [source](<https://arxiv.org/abs/2504.19874>)
- <a id="ref-6"></a>**[6]** A. Blumer, J. Blumer, D. Haussler, R. McConnell, and A. Ehrenfeucht. Complete inverted files for efficient text retrieval and analysis. Journal of the ACM 34(3), 578–595 (1987). [source](<https://doi.org/10.1145/28869.28873>)
- <a id="ref-7"></a>**[7]** Hubert Ramsauer et al. Hopfield Networks is All You Need. arXiv:2008.02217 (2020). [source](<https://arxiv.org/abs/2008.02217>)
- <a id="ref-8"></a>**[8]** Dmitry Krotov and John J. Hopfield. Dense Associative Memory for Pattern Recognition. NeurIPS 29 (2016); arXiv:1606.01164. [source](<https://arxiv.org/abs/1606.01164>)
- <a id="ref-9"></a>**[9]** Subutai Ahmad and Jeff Hawkins. Properties of Sparse Distributed Representations and their Application to Hierarchical Temporal Memory. arXiv:1503.07469 (2015). [source](<https://arxiv.org/abs/1503.07469>)
- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
- <a id="ref-11"></a>**[11]** Jorma Rissanen. Modeling by shortest data description. Automatica 14(5), 465–471 (1978). [source](<https://doi.org/10.1016/0005-1098(78)90005-5>)
- <a id="ref-12"></a>**[12]** Linux kernel contributors. Landlock: unprivileged access control. Linux userspace API documentation (accessed 2026-09-16). [source](<https://www.kernel.org/doc/html/latest/userspace-api/landlock.html>)
- <a id="ref-13"></a>**[13]** bubblewrap contributors. bubblewrap: Low-level unprivileged sandboxing tool used by Flatpak and similar projects. Project README (accessed 2026-09-16). [source](<https://github.com/containers/bubblewrap>)
- <a id="ref-14"></a>**[14]** Subhadeep Pal, Fiona Y. Wang, and Markus J. Buehler. SwarmWorld: Stigmergic technological evolution in societies of language-model agents. arXiv:2608.26081 (2026). [source](<https://arxiv.org/abs/2608.26081>) [source](<https://arxiv.org/html/2608.26081>)
- <a id="ref-15"></a>**[15]** LAMM, MIT. MetaMaterialsDiscovery: Autonomous computational studies of hierarchical metamaterial fracture. Research archive, Hugging Face (accessed 2026-09-16). [source](<https://huggingface.co/lamm-mit/MetaMaterialsDiscovery>)
- <a id="ref-16"></a>**[16]** Di Wu, Hongwei Wang, Wenhao Yu, Yuwei Zhang, Kai-Wei Chang, and Dong Yu. LongMemEval: Benchmarking Chat Assistants on Long-Term Interactive Memory. ICLR (2025); arXiv:2410.10813 (2024). [source](<https://arxiv.org/abs/2410.10813>)
- <a id="ref-17"></a>**[17]** Adyasha Maharana, Dong-Ho Lee, Sergey Tulyakov, Mohit Bansal, Francesco Barbieri, and Yuwei Fang. Evaluating Very Long-Term Conversational Memory of LLM Agents. ACL (2024); arXiv:2402.17753. [source](<https://arxiv.org/abs/2402.17753>) [source](<https://aclanthology.org/2024.acl-long.747/>)
- <a id="ref-18"></a>**[18]** Snap Research. LoCoMo: Evaluating Very Long-Term Conversational Memory of LLM Agents. Dataset and benchmark repository (2024). [source](<https://github.com/snap-research/locomo>)
- <a id="ref-19"></a>**[19]** Tamas Spisak and Karl Friston. Self-orthogonalizing attractor neural networks emerging from the free energy principle. Neurocomputing, article 133472 (2026); arXiv:2505.22749 (2025). [source](<https://arxiv.org/abs/2505.22749>) [source](<https://doi.org/10.1016/j.neucom.2026.133472>)
- <a id="ref-20"></a>**[20]** Gordon H. Bower. Mood and memory. American Psychologist 36(2), 129–148 (1981). [source](<https://doi.org/10.1037/0003-066X.36.2.129>)
- <a id="ref-21"></a>**[21]** Roger Brown and James Kulik. Flashbulb memories. Cognition 5(1), 73–99 (1977). [source](<https://doi.org/10.1016/0010-0277(77)90018-X>)
- <a id="ref-22"></a>**[22]** Charles Packer, Sarah Wooders, Kevin Lin, Vivian Fang, Shishir G. Patil, Ion Stoica, and Joseph E. Gonzalez. MemGPT: Towards LLMs as Operating Systems. arXiv:2310.08560 (2023). [source](<https://arxiv.org/abs/2310.08560>)
- <a id="ref-23"></a>**[23]** Ben Geist. Latent Briefing: Efficient Memory Sharing for Multi-Agent Systems via KV Cache Compaction. Ramp Labs Research (2026-04-10). [source](<https://labs.ramp.com/research/latent-briefing-kv-cache/>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
- <a id="ref-25"></a>**[25]** Craig G. Nevill-Manning and Ian H. Witten. Identifying Hierarchical Structure in Sequences: A linear-time algorithm. Journal of Artificial Intelligence Research 7, 67–82 (1997); arXiv:cs/9709102. [source](<https://arxiv.org/abs/cs/9709102>)
- <a id="ref-26"></a>**[26]** Donald O. Hebb. The Organization of Behavior: A Neuropsychological Theory. Wiley (1949); Psychology Press reissue (2002). [source](<https://www.routledge.com/The-Organization-of-Behavior-A-Neuropsychological-Theory/Hebb/p/book/9780415654531>)
- <a id="ref-27"></a>**[27]** Karl Friston. The free-energy principle: a unified brain theory? Nature Reviews Neuroscience 11, 127–138 (2010). [source](<https://doi.org/10.1038/nrn2787>)
- <a id="ref-28"></a>**[28]** Dan Sperber, Fabrice Clément, Christophe Heintz, Olivier Mascaro, Hugo Mercier, Gloria Origgi, and Deirdre Wilson. Epistemic Vigilance. Mind & Language 25(4), 359–393 (2010). [source](<https://doi.org/10.1111/j.1468-0017.2010.01394.x>)
- <a id="ref-29"></a>**[29]** Noam Shazeer, Azalia Mirhoseini, Krzysztof Maziarz, Andy Davis, Quoc Le, Geoffrey Hinton, and Jeff Dean. Outrageously Large Neural Networks: The Sparsely-Gated Mixture-of-Experts Layer. arXiv:1701.06538 (2017). [source](<https://arxiv.org/abs/1701.06538>)
- <a id="ref-30"></a>**[30]** John G. Cleary and Ian H. Witten. Data Compression Using Adaptive Coding and Partial String Matching. IEEE Transactions on Communications 32(4), 396–402 (1984). [source](<https://doi.org/10.1109/TCOM.1984.1096090>)
- <a id="ref-31"></a>**[31]** Richard S. Sutton. Learning to Predict by the Methods of Temporal Differences. Machine Learning 3, 9–44 (1988). [source](<https://doi.org/10.1023/A:1022633531479>)
- <a id="ref-32"></a>**[32]** Christopher J. C. H. Watkins and Peter Dayan. Q-learning. Machine Learning 8, 279–292 (1992). [source](<https://doi.org/10.1007/BF00992698>)
- <a id="ref-33"></a>**[33]** John J. Hopfield. Neural networks and physical systems with emergent collective computational abilities. PNAS 79(8), 2554–2558 (1982). [source](<https://doi.org/10.1073/pnas.79.8.2554>)
- <a id="ref-35"></a>**[35]** Anthropic. Prompting Claude Fable 5.1. Claude Platform documentation (accessed 2026-09-16). [source](<https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-fable-5-1>)
- <a id="ref-36"></a>**[36]** Anthropic. Skill authoring best practices. Claude Platform documentation (accessed 2026-09-16). [source](<https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices>)
- <a id="ref-41"></a>**[41]** OpenAI. GPT-6 Astra: A new generation of intelligence. OpenAI (2026; exact publication day unverified). [source](<https://openai.com/index/gpt-6-astra/>)
- <a id="ref-53"></a>**[53]** Xinke Jiang et al. AgenticRag-R1: Agentic Reinforcement Learning with Stack Memory for Multi-Step Reasoning, Retrieval and Memorizing. arXiv:2608.29622 (2026). [source](<https://arxiv.org/abs/2608.29622>)
- <a id="ref-58"></a>**[58]** Babasaheb Satpute, Wasudeo P. Rahane, Poonam Pawar, Hrishikesh Vanjari, Rohan Kulkarni, Saurabh Vijay Parhad, and Priyanka V. Deshmukh. Multi agent retrieval validation and knowledge reasoning for enhanced retrieval augmented generation. Scientific Reports (2026). [source](<https://doi.org/10.1038/s41598-026-69002-7>)
- <a id="ref-60"></a>**[60]** Pranay Mahendrakar. Consolidation Without Weights: What the Complementary Learning Systems Analogy Licenses in LLM Agent Memory, and Why the Systems That Borrow Its Name Do Not Inherit Its Guarantee. Zenodo preprint (2026). [source](<https://doi.org/10.5281/zenodo.22699102>)
- <a id="ref-61"></a>**[61]** Rolando Bosch. fidelis: zero-LLM agent memory for Claude Code and AI agents. Zenodo software archive, v0.0.94 (2026). [source](<https://doi.org/10.5281/zenodo.22248259>)
- <a id="ref-62"></a>**[62]** Rolando Bosch. fidelis: zero-LLM agent memory for Claude Code and AI agents. Zenodo software archive, v0.0.97 (2026). [source](<https://doi.org/10.5281/zenodo.22730449>)
- <a id="ref-63"></a>**[63]** Erica Butts and Salam Daher. Consistent Conversational State for Virtual Agents: Slot-Based Memory for Accurate Fact Retrieval. IVA, accepted poster (2026). [source](<https://doi.org/10.1145/3806774.3832787>) [source](<https://iva.acm.org/2026/accepted-papers/>)
- <a id="ref-65"></a>**[65]** Nous Research. Refactoring Hermes with 1,393 agents. Nous Research (2026-09; exact publication day not stated in the article text; describes a run on September 2–4, 2026). [source](<https://nousresearch.com/refactoring-hermes-with-1393-agents>)
- <a id="ref-67"></a>**[67]** A. Blumer, J. Blumer, D. Haussler, A. Ehrenfeucht, M. T. Chen, and J. Seiferas. The smallest automaton recognizing the subwords of a text. Theoretical Computer Science 40, 31–55 (1985). [source](<https://doi.org/10.1016/0304-3975(85)90157-4>)
- <a id="ref-69"></a>**[69]** William R. Thompson. On the likelihood that one unknown probability exceeds another in view of the evidence of two samples. Biometrika 25(3–4), 285–294 (1933). [source](<https://doi.org/10.1093/biomet/25.3-4.285>)
- <a id="ref-70"></a>**[70]** Sergey Brin and Lawrence Page. The Anatomy of a Large-Scale Hypertextual Web Search Engine. Computer Networks 30, 107–117 (1998). [source](<https://research.google/pubs/the-anatomy-of-a-large-scale-hypertextual-web-search-engine/>)
- <a id="ref-71"></a>**[71]** Alex L. Zhang, Tim Kraska, and Omar Khattab. Recursive Language Models. arXiv:2512.24601 (2025; revised 2026). [source](<https://arxiv.org/abs/2512.24601>)
- <a id="ref-72"></a>**[72]** Tianyue Zheng, Weihong Deng, and Jiani Hu. Cross-Age LFW: A Database for Studying Cross-Age Face Recognition in Unconstrained Environments. arXiv:1708.08197 (2017). [source](<https://arxiv.org/abs/1708.08197>)
- <a id="ref-73"></a>**[73]** Aristides Gionis, Piotr Indyk, and Rajeev Motwani. Similarity Search in High Dimensions via Hashing. VLDB, 518–529 (1999). [source](<https://www.vldb.org/conf/1999/P49.pdf>)
<!-- END CITATIONS -->
